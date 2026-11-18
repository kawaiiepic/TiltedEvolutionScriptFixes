#pragma once

#include <Messages/SendChatMessageRequest.h>

#include <Events/PacketEvent.h>
#include <Events/PlayerEnterWorldEvent.h>
#include <map>
#include <set>
#include <vector>
#include <chrono>

struct World;
struct Player;
struct PlayerLeaveEvent;
struct UpdateEvent;

struct PlayerDialogueRequest;
struct TeleportRequest;
struct RequestPlayerHealthUpdate;

/**
 * @brief Dispatches UI events that modify the UI view of other cients.
 */
class OverlayService
{
public:
    OverlayService(World& aWorld, entt::dispatcher& aDispatcher);

protected:
    /**
     * @brief Applies regex on chat message and relays it to other clients.
     */
    void HandleChatMessage(const PacketEvent<SendChatMessageRequest>& acMessage) const noexcept;
    void HandlePlayerJoin(const PlayerEnterWorldEvent& acEvent) const noexcept;
    void OnPlayerDialogue(const PacketEvent<PlayerDialogueRequest>& acMessage) noexcept;
    void OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept;
    void OnTeleport(const PacketEvent<TeleportRequest>& acMessage) const noexcept;
    void OnPlayerHealthUpdate(const PacketEvent<RequestPlayerHealthUpdate>& acMessage) const noexcept;

private:
    struct DialogueSession
    {
        uint32_t Owner{};
        std::map<uint32_t, uint32_t> Participants; // Native menu participants only.
        uint32_t Revision{};
        std::vector<String> Options;
        std::map<uint32_t, uint32_t> Votes; // Player id -> current option index.
    };

    void OnDialogueUpdate(const UpdateEvent&) noexcept;
    void RefreshDialoguePanels() noexcept;
    void InvalidateOptions(DialogueSession& aSession) noexcept;
    bool IsDialogueActorInRange(uint32_t aActorId, const Player& acPlayer) const noexcept;
    void LeaveDialogue(uint32_t aPlayerId) noexcept;
    void SendDialogueState(uint32_t aActorId, const DialogueSession& acSession) const noexcept;

    World& m_world;
    std::map<uint32_t, DialogueSession> m_dialogueSessions;
    std::map<uint32_t, uint32_t> m_playerDialogues; // Player id -> actor id.
    std::map<uint32_t, std::set<uint32_t>> m_visiblePanels;
    uint32_t m_nextDialogueRevision{};
    std::chrono::steady_clock::time_point m_nextPanelRefresh{};
    entt::scoped_connection m_dialogueUpdateConnection;
    entt::scoped_connection m_dialogueLeaveConnection;

    entt::scoped_connection m_chatMessageConnection;
    entt::scoped_connection m_playerDialogueConnection;
    entt::scoped_connection m_teleportConnection;
    entt::scoped_connection m_playerHealthConnection;
};
