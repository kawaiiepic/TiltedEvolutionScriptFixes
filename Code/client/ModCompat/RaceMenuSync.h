#pragma once

struct World;

class RaceMenuSync
{
public:
    RaceMenuSync(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~RaceMenuSync();
    TP_NOCOPYMOVE(RaceMenuSync);

private:
    struct State;
    TiltedPhoques::UniquePtr<State> m_state;
};
