#pragma once

struct World;

// Generic nearby-player speech. Voice frameworks can supply notifications
// through the SKSE bridge even if they bypass the native engine hook.
class PlayerVoiceSync
{
public:
    PlayerVoiceSync(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~PlayerVoiceSync();
    TP_NOCOPYMOVE(PlayerVoiceSync);
    void CaptureNative(const char* apPath) noexcept;

private:
    struct State;
    TiltedPhoques::UniquePtr<State> m_state;
};
