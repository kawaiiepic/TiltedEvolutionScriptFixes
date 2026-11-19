#include <TiltedOnlinePCH.h>

#include "World.h"

#include <Services/DiscoveryService.h>
#include <Services/InputService.h>
#include <Services/TransportService.h>
#include <Services/RunnerService.h>
#include <Services/ImguiService.h>
#include <Services/PapyrusService.h>
#include <Services/DiscordService.h>
#include <Services/ObjectService.h>
#include <Services/QuestService.h>
#include <Services/ActorValueService.h>
#include <Services/InventoryService.h>
#include <Services/MagicService.h>
#include <Services/CommandService.h>
#include <Services/CalendarService.h>
#include <Services/StringCacheService.h>
#include <Services/PlayerService.h>
#include <Services/CombatService.h>
#include <Services/WeatherService.h>
#include <Services/MapService.h>

#include <Events/PreUpdateEvent.h>
#include <Events/UpdateEvent.h>

#include <Games/Skyrim/Interface/UI.h>
#include <Games/Skyrim/BSGraphics/BSGraphicsRenderer.h>
#include <ModCompat/CharacterCheckpoints.h>
#include <ModCompat/BehaviorVar.h>  

World::World()
    : m_runner(m_dispatcher)
    , m_transport(*this, m_dispatcher)
    , m_modSystem(m_dispatcher)
    , m_lastFrameTime{std::chrono::high_resolution_clock::now()}
    , m_gameThreadId{GetCurrentThreadId()}
{
    ctx().emplace<ImguiService>();
    ctx().emplace<DiscoveryService>(*this, m_dispatcher);
    ctx().emplace<OverlayService>(*this, m_transport, m_dispatcher);
    ctx().emplace<InputService>(ctx().at<OverlayService>());
    ctx().emplace<CharacterService>(*this, m_dispatcher, m_transport);
    ctx().emplace<DebugService>(m_dispatcher, *this, m_transport, ctx().at<ImguiService>());
    ctx().emplace<PapyrusService>(m_dispatcher);
    ctx().emplace<DiscordService>(m_dispatcher);
    ctx().emplace<ObjectService>(*this, m_dispatcher, m_transport);
    ctx().emplace<CalendarService>(*this, m_dispatcher, m_transport);
    ctx().emplace<QuestService>(*this, m_dispatcher);
    ctx().emplace<PartyService>(*this, m_dispatcher, m_transport);
    ctx().emplace<ActorValueService>(*this, m_dispatcher, m_transport);
    ctx().emplace<InventoryService>(*this, m_dispatcher, m_transport);
    ctx().emplace<MagicService>(*this, m_dispatcher, m_transport);
    ctx().emplace<CommandService>(*this, m_transport, m_dispatcher);
    ctx().emplace<PlayerService>(*this, m_dispatcher, m_transport);
    ctx().emplace<StringCacheService>(m_dispatcher);
    ctx().emplace<CombatService>(*this, m_transport, m_dispatcher);
    ctx().emplace<WeatherService>(*this, m_transport, m_dispatcher);
    ctx().emplace<MapService>(*this, m_dispatcher, m_transport);

    BehaviorVar::Get()->Init();
}

World::~World() = default;

namespace
{
struct UpdateGuard
{
    std::atomic_flag& Flag;
    ~UpdateGuard() { Flag.clear(std::memory_order_release); }
};
}

void World::Update() noexcept
{
    if (m_updating.test_and_set(std::memory_order_acquire)) return;
    const UpdateGuard guard{m_updating};
    m_lastRegularUpdate.store(GetTickCount64(), std::memory_order_relaxed);
    const auto cNow = std::chrono::high_resolution_clock::now();
    const auto cDelta = cNow - m_lastFrameTime;
    m_lastFrameTime = cNow;

    const auto cDeltaSeconds = std::chrono::duration_cast<std::chrono::duration<double>>(cDelta).count();

    auto& overlay = GetOverlayService();
    overlay.ProcessBrowserReady();
    if (m_hasLoadedCharacter)
    {
        const auto* pUI = UI::Get();
        if (pUI && pUI->GetMenuOpen("Main Menu"))
            m_hasLoadedCharacter = false;
    }
    if (!m_hasLoadedCharacter)
    {
        m_hasLoadedCharacter = m_transport.CanEnterWorld();
        if (!m_hasLoadedCharacter)
        {
            m_runner.OnUpdate(UpdateEvent(cDeltaSeconds));
            m_transport.Poll();
            overlay.UpdateMainMenu();
            return;
        }
    }
    m_dispatcher.trigger(PreUpdateEvent(cDeltaSeconds));

    // Force run this before so we get the tasks scheduled to run
    m_runner.OnUpdate(UpdateEvent(cDeltaSeconds));
    m_dispatcher.trigger(UpdateEvent(cDeltaSeconds));
}

void World::PumpMainMenu() noexcept
{
    if (entt::locator<World>::has_value())
        Get().UpdateMainMenu();
}

void World::SetMenuWindowThread(uint32_t aThreadId) noexcept
{
    if (aThreadId && entt::locator<World>::has_value())
        Get().m_gameThreadId.store(aThreadId, std::memory_order_release);
}

void World::QueueCheckpointTask(std::function<void()> aTask) noexcept
{
    m_checkpointTasks.Add(std::move(aTask));
    BSGraphics::WakeMainMenuPump();
}

void World::UpdateMainMenu() noexcept
{
    // Use only the game thread. A window/engine callback may be nested
    // inside the normal VM update, so serialize the two paths.
    const uint32_t thread = m_gameThreadId.load(std::memory_order_acquire);
    if (GetCurrentThreadId() != thread)
    {
        if (!m_loggedWrongMenuThread.exchange(true))
            spdlog::info("Main-menu pump skipped on thread {}; window owner is {}", GetCurrentThreadId(), thread);
        return;
    }
    if (m_updating.test_and_set(std::memory_order_acquire)) return;
    const UpdateGuard guard{m_updating};
    // The VM can tick on a worker. Only the window owner drains native
    // checkpoint calls, including autosaves after leaving the title screen.
    m_checkpointTasks.Drain();
    auto& overlay = GetOverlayService();
    if (!overlay.GetOverlayApp()) return;
    if (!m_loggedMenuPump)
    {
        m_loggedMenuPump = true;
        spdlog::info("Skyrim Together pre-game pump reached game window thread {}", GetCurrentThreadId());
    }
    // Apply the browser handshake even before the native menu is registered.
    overlay.ProcessBrowserReady();
    const uint64_t now = GetTickCount64();
    if (now - m_lastRegularUpdate.load(std::memory_order_relaxed) < 100 ||
        now - m_lastMenuUpdate < 30 || m_transport.IsOnline()) return;
    m_lastMenuUpdate = now;
    auto* ui = UI::Get();
    if (!ui) return;
    const bool mainMenu = ui->IsMainMenuOpen();
    if (mainMenu) m_hasLoadedCharacter = false;
    if (!mainMenu && !overlay.GetMainMenu() && !CharacterCheckpoints::NeedsStagingPump()) return;

    const auto frame = std::chrono::high_resolution_clock::now();
    const double delta = std::chrono::duration<double>(frame - m_lastFrameTime).count();
    m_lastFrameTime = frame;
    // Only pump queued work, lobby/checkpoint packets and menu UI.
    // Gameplay PreUpdateEvent/UpdateEvent subscribers require a loaded world.
    m_runner.OnUpdate(UpdateEvent(delta));
    m_transport.Poll();
    overlay.UpdateMainMenu();
}

RunnerService& World::GetRunner() noexcept
{
    return m_runner;
}

TransportService& World::GetTransport() noexcept
{
    return m_transport;
}

ModSystem& World::GetModSystem() noexcept
{
    return m_modSystem;
}

uint64_t World::GetTick() const noexcept
{
    return m_transport.GetClock().GetCurrentTick();
}

void World::Create() noexcept
{
    if (!entt::locator<World>::has_value())
    {
        entt::locator<World>::emplace();
    }
}

World& World::Get() noexcept
{
    return entt::locator<World>::value();
}
