#include "InventoryService.h"

#include <Components.h>
#include <World.h>
#include <Services/ObjectService.h>
#include <GameServer.h>

#include <Messages/NotifyObjectInventoryChanges.h>
#include <Messages/RequestInventoryChanges.h>
#include <Messages/NotifyInventoryChanges.h>
#include <Messages/RequestEquipmentChanges.h>
#include <Messages/NotifyEquipmentChanges.h>
#include <Messages/DrawWeaponRequest.h>

#include <Setting.h>
#include <algorithm>
#include <cmath>
namespace
{
Console::Setting bEnableItemDrops{"Gameplay:bEnableItemDrops", "Share player-dropped item stacks until picked up or until the server restarts", false, Console::SettingsFlags::kLocked};
}

bool InventoryService::SharesGroundItems() noexcept
{
    return bEnableItemDrops;
}

InventoryService::InventoryService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
{
    m_groundItemsConnection = aDispatcher.sink<PacketEvent<RequestGroundItems>>().connect<&InventoryService::OnGroundItems>(this);
    m_inventoryChangeConnection = aDispatcher.sink<PacketEvent<RequestInventoryChanges>>().connect<&InventoryService::OnInventoryChanges>(this);
    m_equipmentChangeConnection = aDispatcher.sink<PacketEvent<RequestEquipmentChanges>>().connect<&InventoryService::OnEquipmentChanges>(this);
    m_drawWeaponConnection = aDispatcher.sink<PacketEvent<DrawWeaponRequest>>().connect<&InventoryService::OnWeaponDrawnRequest>(this);
}

void InventoryService::RegisterGroundDrop(const Player& acPlayer, const RequestInventoryChanges& acRequest,
                                          const Inventory& acInventory) noexcept
{
    NotifyGroundItem reply{};
    reply.LocalReference = acRequest.GroundReference;
    reply.Kind = NotifyGroundItem::RejectDrop;

    const auto character = acPlayer.GetCharacter();
    const auto* pMovement = character ? m_world.try_get<MovementComponent>(*character) : nullptr;
    const auto& item = acRequest.Item;
    const auto it = std::find_if(acInventory.Entries.begin(), acInventory.Entries.end(),
        [&](const auto& entry) { return entry.CanBeMerged(item); });
    const auto& mods = m_world.ctx().at<ModsComponent>();
    const auto cellCount = std::count_if(m_groundItems.begin(), m_groundItems.end(),
        [&](const auto& entry) { return entry.second.Cell == acRequest.GroundCell; });
    if (!SharesGroundItems() || !acRequest.Drop || !pMovement || !acRequest.GroundCell ||
        acRequest.GroundCell != acPlayer.GetCellComponent().Cell ||
        !mods.FindFormKey(item.BaseId, acPlayer) || !item.IsPlainStock() ||
        item.Count >= 0 || item.Count < -32767 || it == acInventory.Entries.end() || it->Count < -item.Count ||
        !(glm::distance(pMovement->Position, acRequest.GroundPosition) <= 768.f) ||
        !std::isfinite(acRequest.GroundRotation.x) || !std::isfinite(acRequest.GroundRotation.y) ||
        !std::isfinite(acRequest.GroundRotation.z) || m_groundItems.size() >= 512 || cellCount >= 64)
    {
        acPlayer.Send(reply);
        return;
    }

    NotifyGroundItem drop{};
    drop.Kind = NotifyGroundItem::Spawn;
    drop.Id = ++m_nextGroundId;
    drop.Cell = acRequest.GroundCell;
    drop.Item = item;
    drop.Item.Count = -item.Count;
    drop.Position = acRequest.GroundPosition;
    drop.Rotation = acRequest.GroundRotation;
    m_groundItems.emplace(drop.Id, drop);

    for (const auto* pPlayer : m_world.GetPlayerManager())
        if (pPlayer != &acPlayer && pPlayer->GetCellComponent().Cell == drop.Cell && pPlayer->HasModId(item.BaseId.ModId))
            pPlayer->Send(drop);

    drop.LocalReference = acRequest.GroundReference;
    acPlayer.Send(drop);
}

void InventoryService::OnGroundItems(const PacketEvent<RequestGroundItems>& acMessage) noexcept
{
    const auto& request = acMessage.Packet;
    auto& player = *acMessage.pPlayer;
    const auto character = player.GetCharacter();
    if (!SharesGroundItems() || !character || !request.Cell || request.Cell != player.GetCellComponent().Cell)
        return;

    if (!request.Id)
    {
        for (const auto& [id, drop] : m_groundItems)
            if (drop.Cell == request.Cell && player.HasModId(drop.Item.BaseId.ModId))
                player.Send(drop);

        NotifyGroundItem complete{};
        complete.Kind = NotifyGroundItem::SnapshotComplete;
        complete.Cell = request.Cell;
        player.Send(complete);
        return;
    }

    auto it = m_groundItems.find(request.Id);
    auto* pInventory = m_world.try_get<InventoryComponent>(*character);
    const auto* pMovement = m_world.try_get<MovementComponent>(*character);
    const auto* pState = m_world.try_get<CharacterComponent>(*character);
    NotifyGroundItem update{};
    update.Id = request.Id;

    if (it == m_groundItems.end())
    {
        update.Kind = NotifyGroundItem::Remove;
        player.Send(update);
        return;
    }
    if (it->second.Cell != request.Cell || !player.HasModId(it->second.Item.BaseId.ModId) || !pInventory ||
        !pMovement || !pState || pState->IsDead() || pState->Recovery != CharacterComponent::RecoveryState::Standing ||
        !(glm::distance(pMovement->Position, it->second.Position) <= 768.f))
    {
        update.Kind = NotifyGroundItem::RejectClaim;
        player.Send(update);
        return;
    }

    const auto item = it->second.Item;
    m_groundItems.erase(it);
    pInventory->Content.AddOrRemoveEntry(item);

    update.Kind = NotifyGroundItem::Remove;
    for (const auto* pPlayer : m_world.GetPlayerManager())
        if (pPlayer != &player && pPlayer->GetCellComponent().Cell == request.Cell)
            pPlayer->Send(update);

    update.Kind = NotifyGroundItem::Award;
    update.Item = item;
    player.Send(update);

    if (const auto* pOwner = m_world.try_get<OwnerComponent>(*character))
    {
        NotifyInventoryChanges notify{};
        notify.ServerId = World::ToInteger(*character);
        notify.OwnershipEpoch = pOwner->OwnershipEpoch;
        notify.Item = item;
        GameServer::Get()->SendToPlayersInRange(notify, *character, acMessage.GetSender());
    }
}

void InventoryService::OnInventoryChanges(const PacketEvent<RequestInventoryChanges>& acMessage) noexcept
{
    auto& message = acMessage.Packet;

    auto view = m_world.view<InventoryComponent>();

    const auto it = view.find(static_cast<entt::entity>(message.ServerId));

    if (message.GroundReference)
    {
        const auto character = acMessage.pPlayer->GetCharacter();
        const auto* pOwner = character ? m_world.try_get<OwnerComponent>(*character) : nullptr;
        if (it == view.end() || !character || *character != *it || !pOwner ||
            pOwner->GetOwner() != acMessage.pPlayer || pOwner->OwnershipEpoch != message.OwnershipEpoch)
        {
            NotifyGroundItem reply{};
            reply.Kind = NotifyGroundItem::RejectDrop;
            reply.LocalReference = message.GroundReference;
            acMessage.pPlayer->Send(reply);
            return;
        }
    }
    else if (it == view.end())
        return;

    bool isRemoteNpcInteraction = false;

    const auto* pOwnerComponent = m_world.try_get<OwnerComponent>(*it);
    if (pOwnerComponent)
    {
        const auto* pOwner = pOwnerComponent->GetOwner();
        if (pOwnerComponent->OwnershipEpoch != message.OwnershipEpoch)
        {
            const uint32_t ownerId = pOwner ? pOwner->GetId() : 0;
            spdlog::debug(
                "Rejected inventory change from player {:X} for actor {:X} at stale epoch {}; current owner is {:X} at epoch {}",
                acMessage.pPlayer->GetId(), message.ServerId, message.OwnershipEpoch, ownerId, pOwnerComponent->OwnershipEpoch);
            return;
        }

        if (pOwner != acMessage.pPlayer)
        {
            const auto* pCharacterComponent = m_world.try_get<CharacterComponent>(*it);
            const auto* pCellComponent = m_world.try_get<CellIdComponent>(*it);
            // A non-owner may still change an NPC's inventory through normal gameplay interactions
            // such as pickpocketing or looting. The epoch and range checks keep the interaction tied
            // to the currently visible incarnation of that NPC.
            isRemoteNpcInteraction = pOwner && pCharacterComponent && pCellComponent && !pCharacterComponent->IsPlayer()
                && acMessage.pPlayer->GetCellComponent().IsInRange(*pCellComponent, pCharacterComponent->IsDragon());

            if (!isRemoteNpcInteraction)
            {
                const uint32_t ownerId = pOwner ? pOwner->GetId() : 0;
                spdlog::debug(
                    "Rejected inventory change from player {:X} for actor {:X} because it is owned by player {:X}", acMessage.pPlayer->GetId(), message.ServerId, ownerId);
                return;
            }
        }
    }
    else
    {
        const auto* pCell = m_world.try_get<CellIdComponent>(*it);
        if (!m_world.all_of<ObjectComponent>(*it) || !pCell || !acMessage.pPlayer->GetCharacter() ||
            acMessage.pPlayer->GetCellComponent().Cell != pCell->Cell || message.Item.IsQuestItem)
            return;
        if (message.OwnershipEpoch != 0)
        {
            spdlog::warn(
             "Rejected inventory change from player {:X} because object {:X} unexpectedly carried ownership epoch {}",
                acMessage.pPlayer->GetId(), message.ServerId, message.OwnershipEpoch);
            return;
        }
    }

    auto& inventoryComponent = view.get<InventoryComponent>(*it);
    if (const auto* pObject = m_world.try_get<ObjectComponent>(*it);
        pObject && !m_world.ctx().at<ObjectService>().CommitStockChange(*pObject, inventoryComponent.Content, message.Item))
        return;
    if (message.GroundReference)
        RegisterGroundDrop(*acMessage.pPlayer, message, inventoryComponent.Content);
    inventoryComponent.Content.AddOrRemoveEntry(message.Item);

    if (!message.UpdateClients && !isRemoteNpcInteraction)
        return;

    NotifyInventoryChanges notify;
    notify.ServerId = message.ServerId;
    notify.OwnershipEpoch = message.OwnershipEpoch;
    notify.Item = message.Item;

    notify.Drop = false;

    const entt::entity cOrigin = static_cast<entt::entity>(message.ServerId);
    if (!GameServer::Get()->SendToPlayersInRange(notify, cOrigin, acMessage.GetSender()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void InventoryService::OnEquipmentChanges(const PacketEvent<RequestEquipmentChanges>& acMessage) noexcept
{
    auto& message = acMessage.Packet;

    auto view = m_world.view<InventoryComponent>();

    const auto it = view.find(static_cast<entt::entity>(message.ServerId));

    if (it == view.end())
        return;

    const auto* pOwnerComponent = m_world.try_get<OwnerComponent>(*it);
    if (pOwnerComponent)
    {
        if (pOwnerComponent->GetOwner() != acMessage.pPlayer || pOwnerComponent->OwnershipEpoch != message.OwnershipEpoch)
        {
            const uint32_t ownerId = pOwnerComponent->GetOwner() ? pOwnerComponent->GetOwner()->GetId() : 0;
            spdlog::debug(
                "Rejected equipment change from player {:X} for actor {:X}; current owner is {:X} and requested epoch {} does not match {}",
                acMessage.pPlayer->GetId(), message.ServerId, ownerId, message.OwnershipEpoch, pOwnerComponent->OwnershipEpoch);
            return;
        }
    }
    else if (message.OwnershipEpoch != 0)
    {
        spdlog::warn(
            "Rejected equipment change from player {:X} because object {:X} unexpectedly carried ownership epoch {}",
            acMessage.pPlayer->GetId(), message.ServerId, message.OwnershipEpoch);
        return;
    }

    auto& inventoryComponent = view.get<InventoryComponent>(*it);
    inventoryComponent.Content.UpdateEquipment(message.CurrentInventory);

    NotifyEquipmentChanges notify;
    notify.ServerId = message.ServerId;
    notify.OwnershipEpoch = message.OwnershipEpoch;
    notify.ItemId = message.ItemId;
    notify.EquipSlotId = message.EquipSlotId;
    notify.Count = message.Count;
    notify.Unequip = message.Unequip;
    notify.IsSpell = message.IsSpell;
    notify.IsShout = message.IsShout;

    const entt::entity cOrigin = static_cast<entt::entity>(message.ServerId);
    if (!GameServer::Get()->SendToPlayersInRange(notify, cOrigin, acMessage.GetSender()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void InventoryService::OnWeaponDrawnRequest(const PacketEvent<DrawWeaponRequest>& acMessage) noexcept
{
    auto& message = acMessage.Packet;

    auto characterView = m_world.view<CharacterComponent, OwnerComponent>();
    const auto it = characterView.find(static_cast<entt::entity>(message.Id));

    if (it != std::end(characterView) && characterView.get<OwnerComponent>(*it).GetOwner() == acMessage.pPlayer)
    {
        auto& characterComponent = characterView.get<CharacterComponent>(*it);
        characterComponent.SetWeaponDrawn(message.IsWeaponDrawn);
        spdlog::debug("Updating weapon drawn state {:x}:{}", message.Id, message.IsWeaponDrawn);
    }
}
