#include <Services/ObjectService.h>

#include <GameServer.h>
#include <World.h>
#include <Components.h>

#include <Events/PlayerLeaveCellEvent.h>

#include <Messages/ActivateRequest.h>
#include <Messages/NotifyActivate.h>
#include <Messages/LockChangeRequest.h>
#include <Messages/NotifyLockChange.h>
#include <Messages/AssignObjectsRequest.h>
#include <Messages/AssignObjectsResponse.h>
#include <Messages/ScriptAnimationRequest.h>
#include <Messages/NotifyScriptAnimation.h>

namespace
{
constexpr const char* kLockPoliciesPath = "shared-lock-policies.conf";
constexpr const char* kCellLockPoliciesPath = "shared-lock-cell-policies.conf";
constexpr const char* kLockJournalPath = "shared-locks.log";
}

ObjectService::ObjectService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
    , m_lockPolicies(kLockPoliciesPath)
    , m_cellLockPolicies(kCellLockPoliciesPath)
    , m_lockJournal(kLockJournalPath)
    , m_stockCells("shared-stock-cell-policies.conf")
    , m_stockJournal("shared-stock.log")
{
    if (!m_lockPolicies.IsHealthy())
        spdlog::error("Unable to load persistent lock policies {} (error line {}).", kLockPoliciesPath, m_lockPolicies.ErrorLine());
    if (!m_cellLockPolicies.IsHealthy())
        spdlog::error("Unable to load cell lock policies {} (error line {}).", kCellLockPoliciesPath, m_cellLockPolicies.ErrorLine());
    if (!m_lockJournal.IsHealthy())
        spdlog::error("Unable to load persistent locks {} (error line {}).", kLockJournalPath, m_lockJournal.ErrorLine());

    if (!m_stockCells.IsHealthy() || !m_stockJournal.IsHealthy())
        spdlog::error("Could not load shared-stock-cell-policies.conf or shared-stock.log");

    m_leaveCellConnection = aDispatcher.sink<PlayerLeaveCellEvent>().connect<&ObjectService::OnPlayerLeaveCellEvent>(this);
    m_assignObjectConnection = aDispatcher.sink<PacketEvent<AssignObjectsRequest>>().connect<&ObjectService::OnAssignObjectsRequest>(this);
    m_activateConnection = aDispatcher.sink<PacketEvent<ActivateRequest>>().connect<&ObjectService::OnActivate>(this);
    m_lockChangeConnection = aDispatcher.sink<PacketEvent<LockChangeRequest>>().connect<&ObjectService::OnLockChange>(this);
    m_scriptAnimationConnection = aDispatcher.sink<PacketEvent<ScriptAnimationRequest>>().connect<&ObjectService::OnScriptAnimationRequest>(this);
}

// TODO(cosideci): the cell handling of objects need to be revamped.
// We already store the location and worldspace of the mod through CellIdComponent.
// Clients need a message saying the entity was destroyed.
void ObjectService::OnPlayerLeaveCellEvent(const PlayerLeaveCellEvent& acEvent) noexcept
{
    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer->GetCellComponent().Cell == acEvent.OldCell)
            return;
    }

    auto objectView = m_world.view<ObjectComponent, CellIdComponent>();
    Vector<entt::entity> toDestroy;

    for (auto entity : objectView)
    {
        const auto& cellIdComponent = objectView.get<CellIdComponent>(entity);

        if (cellIdComponent.Cell != acEvent.OldCell)
            continue;

        toDestroy.push_back(entity);
    }

    for (auto& entity : toDestroy)
    {
        m_world.destroy(entity);
    }
}

// NOTE: this whole system kinda relies on all objects in a cell being static.
// This is fine for containers and doors, but if this system is expanded, think of temporaries.
void ObjectService::OnAssignObjectsRequest(const PacketEvent<AssignObjectsRequest>& acMessage) noexcept
{
    auto view = m_world.view<FormIdComponent, ObjectComponent, InventoryComponent, CellIdComponent>();

    AssignObjectsResponse response;
    const auto& mods = m_world.ctx().at<ModsComponent>();

    for (const ObjectData& object : acMessage.Packet.Objects)
    {
        const auto key = mods.FindFormKey(object.Id, *acMessage.pPlayer);
        const auto cellKey = mods.FindFormKey(object.CellId, *acMessage.pPlayer);
        if (!key || !cellKey)
            continue;

        const bool cPersist = m_lockPolicies.IsShared(*key, m_cellLockPolicies.IsShared(*cellKey,
            object.NoResetZone && !object.WorldSpaceId));
        const auto iter = std::find_if(
            std::begin(view), std::end(view),
            [view, id = object.Id](auto entity)
            {
                const auto& formIdComponent = view.get<FormIdComponent>(entity);
                return formIdComponent.Id == id;
            });

        if (iter != std::end(view))
        {
            if (view.get<CellIdComponent>(*iter).Cell != object.CellId)
                continue;
            ObjectData objectData;
            objectData.ServerId = World::ToInteger(*iter);

            auto& formIdComponent = view.get<FormIdComponent>(*iter);
            objectData.Id = formIdComponent.Id;
            objectData.CellId = object.CellId;

            auto& objectComponent = view.get<ObjectComponent>(*iter);
            objectData.CurrentLockData = objectComponent.CurrentLockData;

            auto& inventoryComponent = view.get<InventoryComponent>(*iter);
            objectData.CurrentInventory = inventoryComponent.Content;
            if (objectComponent.CampaignStock)
            {
                if (const auto* pStock = m_stockJournal.Find(*objectComponent.CampaignStock))
                {
                    ApplySavedStock(inventoryComponent.Content, *pStock);
                    objectData.CurrentInventory = inventoryComponent.Content;
                }
            }

            objectData.IsSenderFirst = false;

            response.Objects.push_back(objectData);
        }
        else
        {
            const auto cEntity = m_world.create();

            m_world.emplace<FormIdComponent>(cEntity, object.Id);

            auto& objectComponent = m_world.emplace<ObjectComponent>(cEntity);
            objectComponent.CurrentLockData = object.CurrentLockData;

            m_world.emplace<CellIdComponent>(cEntity, object.CellId, object.WorldSpaceId, object.CurrentCoords);
            auto& inventoryComp = m_world.emplace<InventoryComponent>(cEntity);
            inventoryComp.Content = object.CurrentInventory;
            inventoryComp.Content.RemoveByFilter([](const auto& entry) { return entry.IsQuestItem; });

            ObjectData objectData;
            objectData.Id = object.Id;
            objectData.CellId = object.CellId;
            objectData.ServerId = World::ToInteger(cEntity);
            objectData.IsSenderFirst = true;
            if (cPersist)
            {
                objectComponent.CampaignLock = key;
                if (const auto* pState = m_lockJournal.Find(*key))
                {
                    objectData.HasCampaignLock = true;
                    objectData.CurrentLockData.IsLocked = pState->Locked;
                    objectData.CurrentLockData.LockLevel = pState->Level;
                    objectComponent.CurrentLockData = objectData.CurrentLockData;
                }
            }

            if (object.IsContainer && m_stockCells.IsShared(*cellKey, false))
            {
                objectComponent.CampaignStock = key;
                if (const auto* pStock = m_stockJournal.Find(*key))
                {
                    ApplySavedStock(inventoryComp.Content, *pStock);
                    objectData.CurrentInventory = inventoryComp.Content;
                    objectData.HasCampaignStock = true;
                }
            }
            response.Objects.push_back(objectData);
        }
    }

    if (!response.Objects.empty())
    {
        for (auto& object : response.Objects)
        {
            object.CurrentInventory.RemoveByFilter([&](const auto& entry)
            {
                return !acMessage.pPlayer->HasModId(entry.BaseId.ModId) &&
                       entry.BaseId.ModId != UINT32_MAX;
            });
        }
        acMessage.pPlayer->Send(response);
    }
}

void ObjectService::ApplySavedStock(Inventory& aInventory, const Campaign::ContainerStock& acStock) const noexcept
{
    const auto& mods = m_world.ctx().at<ModsComponent>();
    aInventory.RemoveByFilter([&](const auto& entry)
    {
        return entry.IsPlainStock() && mods.FindFormKey(entry.BaseId).has_value();
    });
    for (const auto& [item, count] : acStock)
    {
        if (const auto id = mods.FindFormId(item))
        {
            Inventory::Entry stack{};
            stack.BaseId = *id;
            stack.Count = count;
            aInventory.Entries.push_back(stack);
        }
    }
}

bool ObjectService::CommitStockChange(const ObjectComponent& acObject, const Inventory& acInventory,
                                      const Inventory::Entry& acChange) noexcept
{
    if (!acObject.CampaignStock || !acChange.IsPlainStock() || !acChange.Count)
        return true;

    const auto& mods = m_world.ctx().at<ModsComponent>();
    const auto item = mods.FindFormKey(acChange.BaseId);
    if (!item)
        return true;
    Campaign::ContainerStock stock;
    if (const auto* pStock = m_stockJournal.Find(*acObject.CampaignStock))
        stock = *pStock;
    else
    {
        for (const auto& entry : acInventory.Entries)
        {
            if (!entry.IsPlainStock() || entry.Count <= 0)
                continue;
            if (const auto key = mods.FindFormKey(entry.BaseId))
            {
                auto& count = stock[*key];
                count = static_cast<int32_t>(std::min<int64_t>(INT32_MAX, static_cast<int64_t>(count) + entry.Count));
            }
        }
    }

    const int64_t cCount = static_cast<int64_t>(stock[*item]) + acChange.Count;
    if (cCount <= 0)
        stock.erase(*item);
    else
        stock[*item] = static_cast<int32_t>(std::min<int64_t>(INT32_MAX, cCount));
    if (!m_stockJournal.Set(*acObject.CampaignStock, std::move(stock)))
    {
        spdlog::warn("Could not commit container stock to shared-stock.log");
        return false;
    }
    return true;
}

void ObjectService::OnActivate(const PacketEvent<ActivateRequest>& acMessage) const noexcept
{
    NotifyActivate notifyActivate;
    notifyActivate.Id = acMessage.Packet.Id;
    notifyActivate.ActivatorId = acMessage.Packet.ActivatorId;
    notifyActivate.PreActivationOpenState = acMessage.Packet.PreActivationOpenState;

    for (auto pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer != acMessage.pPlayer && pPlayer->GetCellComponent().Cell == acMessage.Packet.CellId)
        {
            pPlayer->Send(notifyActivate);
        }
    }
}

void ObjectService::OnLockChange(const PacketEvent<LockChangeRequest>& acMessage) noexcept
{
    NotifyLockChange notifyLockChange;
    notifyLockChange.Id = acMessage.Packet.Id;
    notifyLockChange.IsLocked = acMessage.Packet.IsLocked;
    notifyLockChange.LockLevel = acMessage.Packet.LockLevel;

    const auto objectView = m_world.view<FormIdComponent, ObjectComponent, CellIdComponent>();
    const auto iter = std::find_if(objectView.begin(), objectView.end(), [&](auto entity)
    {
        return objectView.get<FormIdComponent>(entity).Id == acMessage.Packet.Id;
    });

    if (iter != objectView.end())
    {
        const auto& cell = objectView.get<CellIdComponent>(*iter);
        auto& objectComponent = objectView.get<ObjectComponent>(*iter);
        if (cell.Cell != acMessage.Packet.CellId || !cell.IsInRange(acMessage.pPlayer->GetCellComponent(), false))
            return;

        if (objectComponent.CampaignLock)
        {
            if (!acMessage.pPlayer->GetCharacter() || !m_lockJournal.IsHealthy())
                return;
            if (!m_lockJournal.Set(*objectComponent.CampaignLock, {notifyLockChange.IsLocked, notifyLockChange.LockLevel}))
            {
                spdlog::error("Unable to persist lock change in {} (error line {}).", kLockJournalPath, m_lockJournal.ErrorLine());
                return;
            }
        }
        objectComponent.CurrentLockData.IsLocked = notifyLockChange.IsLocked;
        objectComponent.CurrentLockData.LockLevel = notifyLockChange.LockLevel;
    }

    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer == acMessage.pPlayer)
            continue;

        if (pPlayer->GetCellComponent().Cell == acMessage.Packet.CellId)
            pPlayer->Send(notifyLockChange);
    }
}

void ObjectService::OnScriptAnimationRequest(const PacketEvent<ScriptAnimationRequest>& acMessage) noexcept
{
    const auto& packet = acMessage.Packet;
    const auto& mods = m_world.ctx().at<ModsComponent>();
    if (acMessage.pPlayer->GetCellComponent().Cell != packet.Cell ||
        !mods.FindFormKey(packet.Id, *acMessage.pPlayer))
        return;

    NotifyScriptAnimation message{};
    message.Id = packet.Id;
    message.Cell = packet.Cell;
    message.Animation = packet.Animation;
    message.EventName = packet.EventName;

    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer != acMessage.pPlayer && pPlayer->GetCellComponent().Cell == packet.Cell &&
            pPlayer->HasModId(packet.Id.ModId))
            pPlayer->Send(message);
    }
}
