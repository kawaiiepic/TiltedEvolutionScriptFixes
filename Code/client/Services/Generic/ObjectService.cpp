#include <Services/ObjectService.h>

#include <World.h>
#include <Games/Overrides.h>
#include <Events/DisconnectedEvent.h>
#include <Events/UpdateEvent.h>
#include <Events/CellChangeEvent.h>
#include <Events/ActivateEvent.h>
#include <Events/LockChangeEvent.h>
#include <Events/ScriptAnimationEvent.h>
#include <Messages/ServerTimeSettings.h>
#include <Messages/AssignObjectsRequest.h>
#include <Messages/AssignObjectsResponse.h>
#include <Messages/ActivateRequest.h>
#include <Messages/NotifyActivate.h>
#include <Messages/LockChangeRequest.h>
#include <Messages/NotifyLockChange.h>
#include <Messages/ScriptAnimationRequest.h>
#include <Messages/NotifyScriptAnimation.h>

#include <PlayerCharacter.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#include <Forms/BGSEncounterZone.h>
#include <Forms/TESFaction.h>
#include <Games/TES.h>

#include <inttypes.h>
#include <map>
#include <unordered_set>

namespace
{
constexpr uint64_t kObjectAnimationDelayMs = 50;
constexpr uint64_t kMaxAnimationClockSkewMs = 1000;
constexpr size_t kMaxPendingObjectAnimations = 128;
}

ObjectService::ObjectService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport)
    : m_world(aWorld)
    , m_transport(aTransport)
{
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&ObjectService::OnDisconnected>(this);
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&ObjectService::OnUpdate>(this);
    m_cellChangeConnection = aDispatcher.sink<CellChangeEvent>().connect<&ObjectService::OnCellChange>(this);
    m_onActivateConnection = aDispatcher.sink<ActivateEvent>().connect<&ObjectService::OnActivate>(this);
    m_activateConnection = aDispatcher.sink<NotifyActivate>().connect<&ObjectService::OnActivateNotify>(this);
    m_lockChangeConnection = aDispatcher.sink<LockChangeEvent>().connect<&ObjectService::OnLockChange>(this);
    m_lockChangeNotifyConnection = aDispatcher.sink<NotifyLockChange>().connect<&ObjectService::OnLockChangeNotify>(this);
    m_assignObjectConnection = aDispatcher.sink<AssignObjectsResponse>().connect<&ObjectService::OnAssignObjectsResponse>(this);
    m_scriptAnimationConnection = aDispatcher.sink<ScriptAnimationEvent>().connect<&ObjectService::OnScriptAnimationEvent>(this);
    m_scriptAnimationNotifyConnection = aDispatcher.sink<NotifyScriptAnimation>().connect<&ObjectService::OnNotifyScriptAnimation>(this);
}

static bool IsNoResetZone(const TESObjectCELL* apCell) noexcept
{
    const auto* pZone = apCell && apCell->loadedCellData ? apCell->loadedCellData->encounterZone : nullptr;
    if (!pZone)
        return false;
    const auto* pSkyrim = ModManager::Get()->GetByName("Skyrim.esm");
    return pSkyrim && pZone->formID == (static_cast<uint32_t>(pSkyrim->GetId()) << 24 | 0xF90B1u);
}

bool IsPlayerHome(const TESObjectCELL* pCell) noexcept
{
    return IsNoResetZone(pCell) && pCell->formID != 0xEEC55; // Sinderion's Field Lab
}

// Find each loaded faction's containers for a jailed player's belongings and stolen items.
// Do not sync them: each player's items must stay separate (#700).
// Only compare the container pointers; never read through them.
Set<const TESObjectREFR*> GetPlayerStashContainers() noexcept
{
    Set<const TESObjectREFR*> containers{};

    ModManager* pModManager = ModManager::Get();
    if (!pModManager)
        return containers;

    for (const TESFaction* pFaction : pModManager->factions)
    {
        if (!pFaction)
            continue;

        if (pFaction->crimeData.playerInventoryContainer)
            containers.insert(pFaction->crimeData.playerInventoryContainer);

        if (pFaction->crimeData.stolenGoodsContainer)
            containers.insert(pFaction->crimeData.stolenGoodsContainer);
    }

    return containers;
}

bool ShouldSyncObject(const TESObjectREFR* apObject, const Set<const TESObjectREFR*>& acPlayerStashContainers) noexcept
{
    if (!apObject)
        return false;

    if (acPlayerStashContainers.contains(apObject))
        return false;

    // Quest chests that take the player's whole inventory without going through faction crime data.
    switch (apObject->formID)
    {
    case 0x39CF1: // Don't sync the chest in the "Diplomatic Immunity" quest
        return false;
    case 0x3EF03: // ...as well as in the "No One Escapes Cidhna Mine" quest
        return false;
    default:
        return true;
    }
}

static void ApplyLockState(TESObjectREFR& aObject, bool aLocked, uint8_t aLevel) noexcept
{
    Lock* pLock = aObject.GetLock();
    if (!pLock)
    {
        if (!aLocked && !aLevel)
            return;
        pLock = aObject.CreateLock();
        if (!pLock)
        {
            spdlog::error("Failed to create lock for object {:X}", aObject.formID);
            return;
        }
    }
    else if (pLock->IsLocked() == aLocked && pLock->lockLevel == aLevel)
    {
        return;
    }

    pLock->lockLevel = aLevel;
    pLock->SetLock(aLocked);
    aObject.LockChange();
}

void ObjectService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    m_cell = {};
    m_pendingAnimations.clear();
    auto view = m_world.view<ObjectComponent>();
    m_world.destroy(view.begin(), view.end());
}

void ObjectService::OnCellChange(const CellChangeEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    if (m_cell != acEvent.CellId)
        m_pendingAnimations.clear();
    m_cell = acEvent.CellId;
    auto view = m_world.view<ObjectComponent>();
    m_world.destroy(view.begin(), view.end());

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    TESObjectCELL* pCell = pPlayer ? pPlayer->GetParentCellEx() : nullptr;
    if (!pCell)
        return;

    // Player homes should not be synced, so that chest contents,
    // which are often used as storage, are never accidentally wiped.
    if (!World::Get().GetServerSettings().SyncPlayerHomes && IsPlayerHome(pCell))
        return;

    GameId cellId{};
    if (!m_world.GetModSystem().GetServerModId(pCell->formID, cellId))
    {
        spdlog::error("Server cell id not found for cell form id {:X}", pCell->formID);
        return;
    }

    GameId worldSpaceId{};
    if (TESWorldSpace* pWorldSpace = pPlayer->GetWorldSpace())
    {
        if (!m_world.GetModSystem().GetServerModId(pWorldSpace->formID, worldSpaceId))
        {
            spdlog::error("Server world space id not found for world space form id {:X}", pWorldSpace->formID);
            return;
        }
    }

    Vector<FormType> formTypes = {FormType::Container, FormType::Door};
    // Door seemed to be at the wrong form id (29, now 32), verify this.
    Vector<TESObjectREFR*> objects = pCell->GetRefsByFormTypes(formTypes);

    AssignObjectsRequest request{};

    const Set<const TESObjectREFR*> playerStashContainers = GetPlayerStashContainers();
    const bool cNoReset = IsNoResetZone(pCell);

    for (TESObjectREFR* pObject : objects)
    {
        if (!ShouldSyncObject(pObject, playerStashContainers))
        {
            spdlog::warn("Excluding sync for {:X}", pObject->formID);
            continue;
        }

        ObjectData objectData{};
        objectData.CellId = cellId;
        objectData.NoResetZone = cNoReset;
        objectData.WorldSpaceId = worldSpaceId;
        objectData.CurrentCoords = GridCellCoords::CalculateGridCellCoords(pObject->position.x, pObject->position.y);

        if (!m_world.GetModSystem().GetServerModId(pObject->formID, objectData.Id) ||
            m_world.GetModSystem().GetGameId(objectData.Id) != pObject->formID)
        {
            spdlog::error("Server form id not found for object with form id {:X}", pObject->formID);
            continue;
        }

        if (Lock* pLock = pObject->GetLock())
        {
            objectData.CurrentLockData.IsLocked = pLock->IsLocked();
            objectData.CurrentLockData.LockLevel = pLock->lockLevel;
        }

        objectData.IsContainer = pObject->baseForm->formType == FormType::Container;
        if (objectData.IsContainer)
            objectData.CurrentInventory = pObject->GetInventory();

        request.Objects.push_back(objectData);
    }

    m_transport.Send(request);
}

static void RestoreContainerStock(TESObjectREFR& aContainer, const Inventory& acStock) noexcept
{
    const Inventory current = aContainer.GetInventory();
    std::map<uint64_t, Inventory::Entry> deltas;
    Set<uint64_t> protectedItems;
    for (const auto& entry : current.Entries)
    {
        if (!entry.IsPlainStock())
            protectedItems.insert(entry.BaseId.LogFormat());
    }

    const auto accumulate = [&](const Inventory& acInventory, int32_t aDirection)
    {
        for (const auto& entry : acInventory.Entries)
        {
            const uint64_t cKey = entry.BaseId.LogFormat();
            if (!entry.IsPlainStock() || protectedItems.contains(cKey) || entry.Count <= 0)
                continue;
            auto& delta = deltas[cKey];
            delta.BaseId = entry.BaseId;
            delta.Count = static_cast<int32_t>(std::clamp<int64_t>(
                static_cast<int64_t>(delta.Count) + static_cast<int64_t>(entry.Count) * aDirection, -INT32_MAX, INT32_MAX));
        }
    };

    accumulate(current, -1);
    accumulate(acStock, 1);
    ScopedInventoryOverride override;
    for (const auto& [id, delta] : deltas)
    {
        if (delta.Count)
            aContainer.AddOrRemoveItem(delta, true);
    }
}

void ObjectService::OnAssignObjectsResponse(const AssignObjectsResponse& acMessage) noexcept
{
    for (const ObjectData& objectData : acMessage.Objects)
    {
        if (!m_cell || objectData.CellId != m_cell)
            continue;

        const uint32_t cObjectId = World::Get().GetModSystem().GetGameId(objectData.Id);
        TESObjectREFR* pObject = Cast<TESObjectREFR>(TESForm::GetById(cObjectId));
        if (!pObject)
        {
            spdlog::error("Object not found for form id {:X}", objectData.Id);
            continue;
        }

        CreateObjectEntity(pObject->formID, objectData.ServerId);

        if (!objectData.IsSenderFirst || objectData.HasCampaignLock)
            ApplyLockState(*pObject, objectData.CurrentLockData.IsLocked, objectData.CurrentLockData.LockLevel);

        if (objectData.HasCampaignStock && pObject->baseForm->formType == FormType::Container)
        {
            RestoreContainerStock(*pObject, objectData.CurrentInventory);
            continue;
        }

        if (objectData.IsSenderFirst)
            continue;

        if (pObject->baseForm->formType == FormType::Container)
        {
            Inventory currentInventory = pObject->GetInventory();

            if (currentInventory.ContainsQuestItems())
                pObject->SetInventoryRetainingQuestItems(currentInventory, objectData.CurrentInventory);
            else
                pObject->SetInventory(objectData.CurrentInventory);
        }
    }
}

entt::entity ObjectService::CreateObjectEntity(const uint32_t acFormId, const uint32_t acServerId) noexcept
{
    const auto view = m_world.view<FormIdComponent, ObjectComponent>();

    auto it = std::find_if(view.begin(), view.end(), [acServerId, view](entt::entity entity) { return view.get<ObjectComponent>(entity).Id == acServerId; });

    if (it != view.end())
        return *it;

    entt::entity entity = m_world.create();
    spdlog::info("Created object entity, server id: {:X}, form id {:X}", acServerId, acFormId);

    m_world.emplace<FormIdComponent>(entity, acFormId);
    m_world.emplace<ObjectComponent>(entity, acServerId);

    return entity;
}

void ObjectService::OnActivate(const ActivateEvent& acEvent) noexcept
{
    if (!acEvent.pObject || !acEvent.pActivator)
        return;

    if (acEvent.ActivateFlag)
    {
        acEvent.pObject->Activate(acEvent.pActivator, acEvent.Unk1, acEvent.pObjectToGet, acEvent.Count, acEvent.DefaultProcessing);
    }

    if (!m_transport.IsConnected())
        return;

    // Furniture animations are carried by actor actions and graph events.
    // Activating the remote furniture also starts its gameplay scripts.
    if (acEvent.pObject->baseForm && acEvent.pObject->baseForm->formType == FormType::Furniture)
        return;

    if (Lock* pLock = acEvent.pObject->GetLock())
    {
        if (pLock->flags & 0xFF)
            return;
    }

    ActivateRequest request;

    if (!m_world.GetModSystem().GetServerModId(acEvent.pObject->formID, request.Id))
    {
        spdlog::error("Server form id not found for object form id {:X}", acEvent.pObject->formID);
        return;
    }

    TESObjectCELL* pCell = acEvent.pObject->GetParentCellEx();
    if (!pCell)
    {
        spdlog::error("Activated object has no parent cell: {:X}", acEvent.pObject->formID);
        return;
    }

    if (!m_world.GetModSystem().GetServerModId(pCell->formID, request.CellId))
    {
        spdlog::error("Server cell id not found for cell form id {:X}", pCell->formID);
        return;
    }

    auto view = m_world.view<FormIdComponent>();
    const auto pEntity = std::find_if(std::begin(view), std::end(view), [id = acEvent.pActivator->formID, view](entt::entity entity) { return view.get<FormIdComponent>(entity).Id == id; });

    if (pEntity == std::end(view))
    {
        // spdlog::error("Activator entity not found for form id {:X}", acEvent.pActivator->formID);
        return;
    }

    std::optional<uint32_t> serverIdRes = Utils::GetServerId(*pEntity);
    if (!serverIdRes.has_value())
        return;

    request.ActivatorId = serverIdRes.value();
    request.PreActivationOpenState = acEvent.PreActivationOpenState;

    m_transport.Send(request);
}

void ObjectService::OnActivateNotify(const NotifyActivate& acMessage) noexcept
{
    if (!m_cell || acMessage.CellId != m_cell)
        return;

    Actor* pActor = Utils::GetByServerId<Actor>(acMessage.ActivatorId);
    if (!pActor)
    {
        spdlog::error("{}: could not find actor server id {:X}", __FUNCTION__, acMessage.ActivatorId);
        return;
    }

    const uint32_t cObjectId = World::Get().GetModSystem().GetGameId(acMessage.Id);
    TESObjectREFR* pObject = Cast<TESObjectREFR>(TESForm::GetById(cObjectId));
    if (!pObject)
    {
        spdlog::error("Failed to retrieve object to activate.");
        return;
    }

    if (!pObject->baseForm || pObject->baseForm->formType == FormType::Furniture)
        return;

    if (pObject->baseForm->formType == FormType::Door)
    {
        auto remotePreActivationState = static_cast<TESObjectREFR::OpenState>(acMessage.PreActivationOpenState);
        TESObjectREFR::OpenState localState = pObject->GetOpenState();

        if (remotePreActivationState != localState)
        {
            // The doors are unsynced at this point. If we'll Activate the one on our side
            // it'll just continue to be unsynced (open remotely, closed locally and vice versa)
            return;
        }
    }

    // unsure if these flags are the best, but these are passed with the papyrus Activate fn
    // might be an idea to have the client send the flags through NotifyActivate
    pObject->Activate(pActor, 0, nullptr, 1, 0);
}

void ObjectService::OnLockChange(const LockChangeEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    LockChangeRequest request;

    if (!m_world.GetModSystem().GetServerModId(acEvent.FormId, request.Id))
    {
        spdlog::error("Server form id for lock object not found, form id: {:X}", acEvent.FormId);
        return;
    }

    const auto* const pObject = Cast<TESObjectREFR>(TESForm::GetById(acEvent.FormId));
    if (!pObject)
        return;

    TESObjectCELL* pCell = pObject->GetParentCellEx();
    if (!pCell)
    {
        spdlog::error("Activated object has no parent cell: {:X}", pObject->formID);
        return;
    }

    if (!m_world.GetModSystem().GetServerModId(pCell->formID, request.CellId))
    {
        spdlog::error("Server cell id for cell not found, cell form id: {:X}", pCell->formID);
        return;
    }

    request.IsLocked = acEvent.IsLocked;
    request.LockLevel = acEvent.LockLevel;

    m_transport.Send(request);
}

void ObjectService::OnLockChangeNotify(const NotifyLockChange& acMessage) noexcept
{
    if (!m_cell || acMessage.CellId != m_cell)
        return;

    const auto cObjectId = World::Get().GetModSystem().GetGameId(acMessage.Id);
    if (cObjectId == 0)
    {
        spdlog::error("Failed to retrieve object id to (un)lock.");
        return;
    }

    auto* pObject = Cast<TESObjectREFR>(TESForm::GetById(cObjectId));
    if (!pObject)
    {
        spdlog::error("Failed to retrieve object to (un)lock.");
        return;
    }

    ApplyLockState(*pObject, acMessage.IsLocked, acMessage.LockLevel);
}

void ObjectService::OnScriptAnimationEvent(const ScriptAnimationEvent& acEvent) noexcept
{
    ScriptAnimationRequest request{};
    auto& mods = m_world.GetModSystem();
    auto* pObject = Cast<TESObjectREFR>(TESForm::GetById(acEvent.FormID));
    auto* pCell = pObject ? pObject->GetParentCellEx() : nullptr;
    if (!m_transport.IsOnline() || !pCell || !mods.GetServerModId(acEvent.FormID, request.Id) ||
        mods.GetGameId(request.Id) != acEvent.FormID ||
        !mods.GetServerModId(pCell->formID, request.Cell) || request.Cell != m_cell)
        return;
    request.Tick = m_world.GetTick();
    request.EventName = acEvent.EventName;

    m_transport.Send(request);
}

void ObjectService::OnNotifyScriptAnimation(const NotifyScriptAnimation& acMessage) noexcept
{
    if (!m_cell || acMessage.Cell != m_cell || acMessage.EventName.empty() ||
        m_pendingAnimations.size() >= kMaxPendingObjectAnimations)
        return;

    const uint64_t cTick = m_world.GetTick();
    if (cTick && acMessage.Tick &&
        (acMessage.Tick > cTick ? acMessage.Tick - cTick : cTick - acMessage.Tick) > kMaxAnimationClockSkewMs)
        return;

    m_pendingAnimations.push_back({acMessage.Id, acMessage.EventName, acMessage.Tick,
                                   std::chrono::steady_clock::now() + std::chrono::milliseconds(750)});
}

void ObjectService::OnUpdate(const UpdateEvent&) noexcept
{
    if (m_pendingAnimations.empty())
        return;

    const auto now = std::chrono::steady_clock::now();
    const uint64_t cTick = m_world.GetTick();
    const auto& mods = m_world.GetModSystem();
    const uint32_t cCellFormId = mods.GetGameId(m_cell);
    std::unordered_set<GameId> waiting;

    for (auto it = m_pendingAnimations.begin(); it != m_pendingAnimations.end();)
    {
        if (now >= it->ExpiresAt)
        {
            it = m_pendingAnimations.erase(it);
            continue;
        }

        if (waiting.contains(it->Id))
        {
            ++it;
            continue;
        }

        const uint32_t cFormId = mods.GetGameId(it->Id);
        auto* pObject = cFormId ? Cast<TESObjectREFR>(TESForm::GetById(cFormId)) : nullptr;
        auto* pCell = pObject ? pObject->GetParentCellEx() : nullptr;
        const bool cDue = !cTick || !it->Tick || (cTick >= it->Tick && cTick - it->Tick >= kObjectAnimationDelayMs);
        if (!cDue || !pCell || pCell->formID != cCellFormId || !pObject->GetNiNode())
        {
            waiting.insert(it->Id);
            ++it;
            continue;
        }

        if (!Cast<Actor>(pObject))
        {
            BSFixedString eventName(it->EventName.c_str());
            pObject->PlayAnimation(&eventName);
            eventName.Release();
        }
        it = m_pendingAnimations.erase(it);
    }
}
