#pragma once

#include <Events/PacketEvent.h>
#include <Campaign/FormSyncPolicies.h>
#include <Campaign/LockJournal.h>
#include <Campaign/ContainerJournal.h>
#include <Structs/Inventory.h>

struct World;
struct ObjectComponent;
struct PlayerLeaveCellEvent;
struct ActivateRequest;
struct LockChangeRequest;
struct AssignObjectsRequest;
struct ScriptAnimationRequest;

/**
 * @brief Manages (interactive) objects and relays interactions with said objects.
 */
class ObjectService
{
public:
    ObjectService(World& aWorld, entt::dispatcher& aDispatcher);
    bool CommitStockChange(const ObjectComponent& acObject, const Inventory& acInventory,
                           const Inventory::Entry& acChange) noexcept;

private:
    void ApplySavedStock(Inventory& aInventory, const Campaign::ContainerStock& acStock) const noexcept;
    void OnPlayerLeaveCellEvent(const PlayerLeaveCellEvent& acEvent) noexcept;
    void OnAssignObjectsRequest(const PacketEvent<AssignObjectsRequest>&) noexcept;
    void OnActivate(const PacketEvent<ActivateRequest>&) const noexcept;
    void OnLockChange(const PacketEvent<LockChangeRequest>&) noexcept;
    void OnScriptAnimationRequest(const PacketEvent<ScriptAnimationRequest>&) noexcept;

    World& m_world;
    Campaign::FormSyncPolicies m_lockPolicies;
    Campaign::FormSyncPolicies m_cellLockPolicies;
    Campaign::LockJournal m_lockJournal;
    Campaign::FormSyncPolicies m_stockCells;
    Campaign::ContainerJournal m_stockJournal;

    entt::scoped_connection m_leaveCellConnection;
    entt::scoped_connection m_assignObjectConnection;
    entt::scoped_connection m_activateConnection;
    entt::scoped_connection m_lockChangeConnection;
    entt::scoped_connection m_scriptAnimationConnection;
};
