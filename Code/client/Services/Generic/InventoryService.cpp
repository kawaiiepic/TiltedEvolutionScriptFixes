#include <Services/InventoryService.h>

#include <Messages/RequestObjectInventoryChanges.h>
#include <Messages/NotifyObjectInventoryChanges.h>
#include <Messages/RequestInventoryChanges.h>
#include <Messages/NotifyInventoryChanges.h>
#include <Messages/RequestEquipmentChanges.h>
#include <Messages/NotifyEquipmentChanges.h>
#include <Messages/DrawWeaponRequest.h>
#include <Messages/NotifyDrawWeapon.h>

#include <Events/UpdateEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/InventoryChangeEvent.h>
#include <Events/EquipmentChangeEvent.h>

#include <World.h>
#include <Games/Skyrim/Interface/UI.h>
#include <PlayerCharacter.h>
#include <Forms/TESObjectCELL.h>
#include <Actor.h>
#include <Structs/ObjectData.h>
#include <Forms/TESWorldSpace.h>
#include <Games/TES.h>
#include <Games/Overrides.h>
#include <EquipManager.h>
#include <Games/ActorExtension.h>
#include <Forms/TESNPC.h>
#include <DefaultObjectManager.h>

InventoryService::InventoryService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_dispatcher(aDispatcher)
    , m_transport(aTransport)
{
    m_groundItemConnection = aDispatcher.sink<NotifyGroundItem>().connect<&InventoryService::OnGroundItem>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&InventoryService::OnDisconnected>(this);
    m_updateConnection = m_dispatcher.sink<UpdateEvent>().connect<&InventoryService::OnUpdate>(this);
    m_inventoryConnection = m_dispatcher.sink<InventoryChangeEvent>().connect<&InventoryService::OnInventoryChangeEvent>(this);
    m_equipmentConnection = m_dispatcher.sink<EquipmentChangeEvent>().connect<&InventoryService::OnEquipmentChangeEvent>(this);
    m_inventoryChangeConnection = m_dispatcher.sink<NotifyInventoryChanges>().connect<&InventoryService::OnNotifyInventoryChanges>(this);
    m_equipmentChangeConnection = m_dispatcher.sink<NotifyEquipmentChanges>().connect<&InventoryService::OnNotifyEquipmentChanges>(this);
}

void InventoryService::OnUpdate(const UpdateEvent& acUpdateEvent) noexcept
{
    RunWeaponStateUpdates();
    RunGroundItemUpdates();
}

bool InventoryService::IsShareableGroundItem(const TESForm& acForm) noexcept
{
    switch (acForm.formType)
    {
    case FormType::Scroll:
    case FormType::Armor:
    case FormType::Ingredient:
    case FormType::Misc:
    case FormType::Weapon:
    case FormType::Ammo:
    case FormType::Alchemy:
        return !acForm.IsTemporary();
    default:
        return false;
    }
}

void InventoryService::TrackGroundDrop(uint32_t aFormId, const GameId& acCell) noexcept
{
    std::lock_guard lock(m_groundMutex);
    m_groundReferences.insert_or_assign(aFormId, GroundReference{0, acCell});
}

bool InventoryService::IsTrackedGroundItem(uint32_t aFormId) noexcept
{
    std::lock_guard lock(m_groundMutex);
    return m_groundReferences.contains(aFormId);
}

bool InventoryService::ClaimGroundItem(uint32_t aFormId, bool aLocalPlayer) noexcept
{
    RequestGroundItems request{};
    {
        std::lock_guard lock(m_groundMutex);
        const auto it = m_groundReferences.find(aFormId);
        if (it == m_groundReferences.end())
            return false;
        if (!aLocalPlayer || !it->second.Id || !m_groundClaims.insert(it->second.Id).second)
            return true;
        request.Id = it->second.Id;
        request.Cell = it->second.Cell;
    }

    m_world.GetRunner().Queue([this, request]()
    {
        if (m_groundCell != request.Cell || !m_transport.Send(request))
        {
            std::lock_guard lock(m_groundMutex);
            m_groundClaims.erase(request.Id);
        }
    });
    return true;
}

void InventoryService::RemoveGroundReferences(uint64_t aId, bool aAll) noexcept
{
    Vector<uint32_t> references;
    {
        std::lock_guard lock(m_groundMutex);
        for (auto it = m_groundReferences.begin(); it != m_groundReferences.end();)
        {
            if (it->second.Id && (aAll || it->second.Id == aId))
            {
                references.push_back(it->first);
                it = m_groundReferences.erase(it);
            }
            else
                ++it;
        }
        if (aAll)
            m_groundClaims.clear();
        else
            m_groundClaims.erase(aId);
    }

    for (const auto formId : references)
        if (auto* pRef = Cast<TESObjectREFR>(TESForm::GetById(formId)); pRef && pRef->IsTemporary())
        {
            pRef->Disable();
            pRef->Delete();
        }
}

void InventoryService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    RemoveGroundReferences(0, true);
    m_groundItems.clear();
    m_failedGroundSpawns.clear();
    m_groundCell = {};
    m_groundSnapshotComplete = false;
    m_nextGroundQuery = {};
    std::lock_guard lock(m_groundMutex);
    m_groundReferences.clear();
}

void InventoryService::OnGroundItem(const NotifyGroundItem& acMessage) noexcept
{
    if (acMessage.Kind == NotifyGroundItem::SnapshotComplete)
    {
        if (acMessage.Cell == m_groundCell)
            m_groundSnapshotComplete = true;
        return;
    }

    if (acMessage.Kind == NotifyGroundItem::RejectDrop)
    {
        std::lock_guard lock(m_groundMutex);
        if (const auto it = m_groundReferences.find(acMessage.LocalReference); it != m_groundReferences.end() && !it->second.Id)
            m_groundReferences.erase(it);
        return;
    }

    if (acMessage.Kind == NotifyGroundItem::RejectClaim)
    {
        std::lock_guard lock(m_groundMutex);
        m_groundClaims.erase(acMessage.Id);
        return;
    }

    if (acMessage.Kind == NotifyGroundItem::Remove || acMessage.Kind == NotifyGroundItem::Award)
    {
        m_groundItems.erase(acMessage.Id);
        m_failedGroundSpawns.erase(acMessage.Id);
        RemoveGroundReferences(acMessage.Id);
        if (acMessage.Kind == NotifyGroundItem::Award)
            if (auto* pPlayer = PlayerCharacter::Get())
            {
                ScopedInventoryOverride override;
                pPlayer->AddOrRemoveItem(acMessage.Item);
            }
        return;
    }

    if (acMessage.Kind != NotifyGroundItem::Spawn || !acMessage.Id || acMessage.Item.Count <= 0 ||
        acMessage.Item.Count > 32767 || !acMessage.Item.IsPlainStock())
        return;

    if (acMessage.LocalReference)
    {
        TESObjectREFR* pOriginal = nullptr;
        {
            std::lock_guard lock(m_groundMutex);
            const auto it = m_groundReferences.find(acMessage.LocalReference);
            if (it != m_groundReferences.end() && !it->second.Id && it->second.Cell == acMessage.Cell)
            {
                pOriginal = Cast<TESObjectREFR>(TESForm::GetById(it->first));
                if (pOriginal && pOriginal->IsTemporary())
                    it->second.Id = acMessage.Id;
                else
                {
                    pOriginal = nullptr;
                    m_groundReferences.erase(it);
                }
            }
        }
        if (pOriginal)
        {
            pOriginal->SetSkipSaveFlag(true);
            if (acMessage.Cell != m_groundCell)
                RemoveGroundReferences(acMessage.Id);
        }
    }

    if (acMessage.Cell == m_groundCell)
        m_groundItems.insert_or_assign(acMessage.Id, acMessage);
}

void InventoryService::RunGroundItemUpdates() noexcept
{
    if (!m_transport.IsOnline() || !m_world.GetServerSettings().ShareGroundItems)
        return;

    const auto* pPlayer = PlayerCharacter::Get();
    auto* pCell = pPlayer ? pPlayer->GetParentCellEx() : nullptr;
    GameId cell{};
    if (!pCell || !m_world.GetModSystem().GetServerModId(pCell->formID, cell))
        return;

    if (cell != m_groundCell)
    {
        RemoveGroundReferences(0, true);
        m_groundItems.clear();
        m_failedGroundSpawns.clear();
        m_groundCell = cell;
        m_groundSnapshotComplete = false;
        m_nextGroundQuery = {};
    }

    const auto now = std::chrono::steady_clock::now();
    if (!m_groundSnapshotComplete && now >= m_nextGroundQuery)
    {
        RequestGroundItems request{};
        request.Cell = cell;
        m_transport.Send(request);
        m_nextGroundQuery = now + std::chrono::seconds(1);
    }
    SpawnGroundReferences(*pCell);
}

void InventoryService::SpawnGroundReferences(TESObjectCELL& aCell) noexcept
{
    if (!aCell.IsAttached())
        return;
    const auto& mods = m_world.GetModSystem();
    for (const auto& [id, drop] : m_groundItems)
    {
        if (m_failedGroundSpawns.contains(id))
            continue;
        {
            std::lock_guard lock(m_groundMutex);
            if (std::any_of(m_groundReferences.begin(), m_groundReferences.end(),
                [id](const auto& entry) { return entry.second.Id == id; }))
                continue;
        }

        auto* pBase = TESForm::GetById(mods.GetGameId(drop.Item.BaseId));
        if (!pBase || !IsShareableGroundItem(*pBase))
        {
            m_failedGroundSpawns.insert(id);
            continue;
        }

        NiPoint3 position(drop.Position);
        NiPoint3 rotation(drop.Rotation);
        const uint32_t handle = ModManager::Get()->SpawnGroundItem(pBase, position, rotation, &aCell);
        auto* pRef = handle ? TESObjectREFR::GetByHandle(handle) : nullptr;
        if (!pRef || !pRef->IsTemporary())
        {
            m_failedGroundSpawns.insert(id);
            continue;
        }

        pRef->SetSkipSaveFlag(true);
        if (drop.Item.Count > 1)
            pRef->extraData.SetStackCount(static_cast<uint16_t>(drop.Item.Count));
        std::lock_guard lock(m_groundMutex);
        m_groundReferences.emplace(pRef->formID, GroundReference{id, drop.Cell});
    }
}

void InventoryService::OnInventoryChangeEvent(const InventoryChangeEvent& acEvent) noexcept
{
    const auto releasePendingDrop = [&]()
    {
        if (acEvent.GroundReference)
        {
            std::lock_guard lock(m_groundMutex);
            m_groundReferences.erase(acEvent.GroundReference);
        }
    };
    if (!m_transport.IsConnected())
    {
        releasePendingDrop();
        return;
    }

    auto view = m_world.view<FormIdComponent>();

    const auto iter = std::find_if(std::begin(view), std::end(view), [view, formId = acEvent.FormId](auto entity) { return view.get<FormIdComponent>(entity).Id == formId; });

    if (iter == std::end(view))
    {
        releasePendingDrop();
        return;
    }

    uint32_t serverId = 0;
    if (acEvent.OwnershipEpoch != 0)
    {
        const auto* pLocalComponent = m_world.try_get<LocalComponent>(*iter);
        const auto* pRemoteComponent = m_world.try_get<RemoteComponent>(*iter);
        const bool ownershipMatches = (pLocalComponent && pLocalComponent->Id == acEvent.ServerId && pLocalComponent->OwnershipEpoch == acEvent.OwnershipEpoch)
            || (pRemoteComponent && pRemoteComponent->Id == acEvent.ServerId && pRemoteComponent->OwnershipEpoch == acEvent.OwnershipEpoch);

        if (!ownershipMatches)
        {
            spdlog::debug("Discarded an inventory change for actor {:X} because ownership changed after it was queued (epoch {})", acEvent.ServerId, acEvent.OwnershipEpoch);
            releasePendingDrop();
            return;
        }

        serverId = acEvent.ServerId;
    }
    else
    {
        if (Cast<Actor>(TESForm::GetById(acEvent.FormId)))
            return;

        const std::optional<uint32_t> serverIdRes = Utils::GetServerId(*iter);
        if (!serverIdRes)
        {
            spdlog::warn(
                "Discarded inventory change for form {:X} because it has no server entity (item {:X}, count {})", acEvent.FormId, acEvent.Item.BaseId.BaseId, acEvent.Item.Count);
            return;
        }
        serverId = *serverIdRes;
    }

    RequestInventoryChanges request;
    request.ServerId = serverId;
    request.OwnershipEpoch = acEvent.OwnershipEpoch;
    request.Item = acEvent.Item;
    request.Drop = acEvent.Drop;
    request.UpdateClients = acEvent.UpdateClients;

    request.GroundReference = acEvent.GroundReference;
    request.GroundCell = acEvent.GroundCell;
    request.GroundPosition = acEvent.GroundPosition;
    request.GroundRotation = acEvent.GroundRotation;

    if (!m_transport.Send(request))
        releasePendingDrop();

    spdlog::info("Sending item request, item: {:X}, count: {}, target object: {:X}", acEvent.Item.BaseId.BaseId, acEvent.Item.Count, acEvent.FormId);
}

void InventoryService::OnEquipmentChangeEvent(const EquipmentChangeEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    auto view = m_world.view<FormIdComponent>();

    const auto iter = std::find_if(std::begin(view), std::end(view), [view, formId = acEvent.ActorId](auto entity) { return view.get<FormIdComponent>(entity).Id == formId; });

    if (iter == std::end(view))
        return;

    const auto* pLocalComponent = m_world.try_get<LocalComponent>(*iter);
    if (acEvent.OwnershipEpoch == 0 || !pLocalComponent || pLocalComponent->Id != acEvent.ServerId || pLocalComponent->OwnershipEpoch != acEvent.OwnershipEpoch)
    {
        spdlog::debug("Discarded an equipment change for actor {:X} because ownership changed after it was queued (epoch {})", acEvent.ServerId, acEvent.OwnershipEpoch);
        return;
    }

    Actor* pActor = Cast<Actor>(TESForm::GetById(acEvent.ActorId));
    if (!pActor)
        return;

    auto& modSystem = World::Get().GetModSystem();

    RequestEquipmentChanges request;
    request.ServerId = acEvent.ServerId;
    request.OwnershipEpoch = acEvent.OwnershipEpoch;

    if (!modSystem.GetServerModId(acEvent.EquipSlotId, request.EquipSlotId))
        return;
    if (!modSystem.GetServerModId(acEvent.ItemId, request.ItemId))
        return;

    request.Count = acEvent.Count;
    request.Unequip = acEvent.Unequip;
    request.IsSpell = acEvent.IsSpell;
    request.IsShout = acEvent.IsShout;
    request.IsAmmo = acEvent.IsAmmo;
    request.CurrentInventory = pActor->GetEquipment();

    m_transport.Send(request);

    spdlog::info("Sending equipment request, item: {:X}, count: {}, target object: {:X}", acEvent.ItemId, acEvent.Count, acEvent.ActorId);
}

void InventoryService::OnNotifyInventoryChanges(const NotifyInventoryChanges& acMessage) noexcept
{
    if (acMessage.OwnershipEpoch != 0)
    {
        Actor* pActor = nullptr;

        auto remoteView = m_world.view<RemoteComponent, FormIdComponent>(entt::exclude<LocalComponent>);
        const auto remoteIt = std::find_if(remoteView.begin(), remoteView.end(), [remoteView, &acMessage](const entt::entity aEntity)
        {
            const auto& remoteComponent = remoteView.get<RemoteComponent>(aEntity);
            return remoteComponent.Id == acMessage.ServerId && remoteComponent.OwnershipEpoch == acMessage.OwnershipEpoch;
        });

        if (remoteIt != remoteView.end())
            pActor = Cast<Actor>(TESForm::GetById(remoteView.get<FormIdComponent>(*remoteIt).Id));
        else
        {
            auto localView = m_world.view<LocalComponent, FormIdComponent>();
            const auto localIt = std::find_if(localView.begin(), localView.end(), [localView, &acMessage](const entt::entity aEntity)
            {
                const auto& localComponent = localView.get<LocalComponent>(aEntity);
                return localComponent.Id == acMessage.ServerId && localComponent.OwnershipEpoch == acMessage.OwnershipEpoch;
            });

            if (localIt != localView.end())
                pActor = Cast<Actor>(TESForm::GetById(localView.get<FormIdComponent>(*localIt).Id));
        }

        if (!pActor)
        {
            spdlog::debug("Discarded an inventory update for actor {:X} because epoch {} is no longer current", acMessage.ServerId, acMessage.OwnershipEpoch);
            return;
        }

        ScopedInventoryOverride _;

        if (acMessage.Drop)
            pActor->DropOrPickUpObject(acMessage.Item, nullptr, nullptr);
        else
            pActor->AddOrRemoveItem(acMessage.Item);

        return;
    }

    TESObjectREFR* pObject = Utils::GetByServerId<TESObjectREFR>(acMessage.ServerId);
    if (!pObject || acMessage.Item.IsQuestItem)
        return;

    if (acMessage.Item.Count < 0)
    {
        const auto inventory = pObject->GetInventory();
        if (std::any_of(inventory.Entries.begin(), inventory.Entries.end(), [&](const auto& entry)
            { return entry.IsQuestItem && entry.BaseId == acMessage.Item.BaseId; }))
            return;
    }

    ScopedInventoryOverride _;
    pObject->AddOrRemoveItem(acMessage.Item);
}

void InventoryService::OnNotifyEquipmentChanges(const NotifyEquipmentChanges& acMessage) noexcept
{
    auto view = m_world.view<RemoteComponent, FormIdComponent>(entt::exclude<LocalComponent>);
    const auto it = std::find_if(view.begin(), view.end(), [view, &acMessage](const entt::entity aEntity)
    {
        const auto& remoteComponent = view.get<RemoteComponent>(aEntity);
        return remoteComponent.Id == acMessage.ServerId && remoteComponent.OwnershipEpoch == acMessage.OwnershipEpoch;
    });
    if (it == view.end())
    {
        spdlog::debug("Discarded an equipment update for actor {:X} because epoch {} is no longer current", acMessage.ServerId, acMessage.OwnershipEpoch);
        return;
    }

    Actor* pActor = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(*it).Id));
    if (!pActor)
        return;

    auto& modSystem = World::Get().GetModSystem();

    uint32_t itemId = modSystem.GetGameId(acMessage.ItemId);
    TESForm* pItem = TESForm::GetById(itemId);

    if (!pItem)
    {
        spdlog::error("Could not find inventory item {:X}:{:X}", acMessage.ItemId.ModId, acMessage.ItemId.BaseId);
        return;
    }

    uint32_t equipSlotId = modSystem.GetGameId(acMessage.EquipSlotId);
    TESForm* pEquipSlot = TESForm::GetById(equipSlotId);

    uint32_t slotId = 0;
    if (pEquipSlot == DefaultObjectManager::Get().rightEquipSlot)
        slotId = 1;

    auto* pEquipManager = EquipManager::Get();

    if (acMessage.IsSpell)
    {
        if (acMessage.Unequip)
            pEquipManager->UnEquipSpell(pActor, pItem, slotId);
        else
            pEquipManager->EquipSpell(pActor, pItem, slotId);

        return;
    }
    else if (acMessage.IsShout)
    {
        if (acMessage.Unequip)
            pEquipManager->UnEquipShout(pActor, pItem);
        else
            pEquipManager->EquipShout(pActor, pItem);

        return;
    }

    // TODO: ExtraData necessary? probably
    if (acMessage.Unequip)
    {
        pEquipManager->UnEquip(pActor, pItem, nullptr, acMessage.Count, pEquipSlot, false, true, false, false, nullptr);
    }
    else
    {
        // Unequip all armor first, since the game won't auto unequip armor
        Inventory wornArmor{};
        if (pItem->formType == FormType::Armor)
        {
            wornArmor = pActor->GetWornArmor();
            for (const auto& armor : wornArmor.Entries)
            {
                uint32_t armorId = modSystem.GetGameId(armor.BaseId);
                TESForm* pArmor = TESForm::GetById(armorId);
                if (pArmor)
                    pEquipManager->UnEquip(pActor, pArmor, nullptr, 1, pEquipSlot, false, true, false, false, nullptr);
            }
        }

        pEquipManager->Equip(pActor, pItem, nullptr, acMessage.Count, pEquipSlot, false, true, false, false);

        for (const auto& armor : wornArmor.Entries)
        {
            uint32_t armorId = modSystem.GetGameId(armor.BaseId);
            TESForm* pArmor = TESForm::GetById(armorId);
            if (pArmor)
                pEquipManager->Equip(pActor, pArmor, nullptr, 1, pEquipSlot, false, true, false, false);
        }
    }
}

void InventoryService::RunWeaponStateUpdates() noexcept
{
    if (!m_transport.IsConnected())
        return;

    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 500ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    auto view = m_world.view<FormIdComponent, LocalComponent>();

    for (auto entity : view)
    {
        const auto& formIdComponent = view.get<FormIdComponent>(entity);
        Actor* const pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
        auto& localComponent = view.get<LocalComponent>(entity);

        bool isWeaponDrawn = pActor->actorState.IsWeaponDrawn();
        if (isWeaponDrawn != localComponent.IsWeaponDrawn)
        {
            localComponent.IsWeaponDrawn = isWeaponDrawn;

            DrawWeaponRequest request;
            request.Id = localComponent.Id;
            request.IsWeaponDrawn = isWeaponDrawn;

            m_transport.Send(request);
        }
    }
}
