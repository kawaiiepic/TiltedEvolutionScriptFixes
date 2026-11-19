#pragma once

#include <Events/PacketEvent.h>
#include <Messages/GroundItems.h>
#include <map>

struct World;
struct UpdateEvent;
struct RequestObjectInventoryChanges;
struct RequestInventoryChanges;
struct RequestEquipmentChanges;
struct DrawWeaponRequest;
struct PlayerLeaveCellEvent;

/**
 * @brief Relays inventory/equipment changes and updates the server side state.
 */
class InventoryService
{
public:
    InventoryService(World& aWorld, entt::dispatcher& aDispatcher);
    static bool SharesGroundItems() noexcept;

    /**
     * @brief Relays inventory changes to other clients and updates server side inventories.
     */
    void OnInventoryChanges(const PacketEvent<RequestInventoryChanges>& acMessage) noexcept;
    /**
     * @brief Relays equipment changes to other clients and updates server side equipment.
     */
    void OnEquipmentChanges(const PacketEvent<RequestEquipmentChanges>& acMessage) noexcept;
    /**
     * @brief Relays weapon draw changes to other clients and updates server side weapon draw state.
     */
    void OnWeaponDrawnRequest(const PacketEvent<DrawWeaponRequest>& acMessage) noexcept;

private:
    void OnGroundItems(const PacketEvent<RequestGroundItems>& acMessage) noexcept;
    void RegisterGroundDrop(const Player& acPlayer, const RequestInventoryChanges& acRequest,
                            const Inventory& acInventory) noexcept;

    std::map<uint64_t, NotifyGroundItem> m_groundItems;
    uint64_t m_nextGroundId{};
    entt::scoped_connection m_groundItemsConnection;

    World& m_world;

    entt::scoped_connection m_inventoryChangeConnection;
    entt::scoped_connection m_equipmentChangeConnection;
    entt::scoped_connection m_drawWeaponConnection;
};
