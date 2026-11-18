#pragma once

// UI-only notifications; never trigger world join/leave handlers for a
// connection that has not entered a Skyrim world.
struct ServerLobbyEvent
{
    bool Authenticated{};
};
