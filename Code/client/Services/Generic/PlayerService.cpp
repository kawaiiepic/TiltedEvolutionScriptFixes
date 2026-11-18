#include <Services/PlayerService.h>

#include <World.h>
#include <Components.h>

#include <Events/ActivateEvent.h>
#include <Events/RevivePlayerEvent.h>
#include <Events/UpdateEvent.h>
#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/GridCellChangeEvent.h>
#include <Events/CellChangeEvent.h>
#include <Events/PlayerLevelEvent.h>
#include <Events/PartyJoinedEvent.h>
#include <Events/PartyLeftEvent.h>
#include <Events/BeastFormChangeEvent.h>

#include <Messages/PlayerRespawnRequest.h>
#include <Messages/NotifyPlayerDowned.h>
#include <Messages/NotifyPlayerRespawn.h>
#include <Messages/ShiftGridCellRequest.h>
#include <Messages/EnterExteriorCellRequest.h>
#include <Messages/EnterInteriorCellRequest.h>
#include <Messages/PlayerLevelRequest.h>
#include <Messages/RequestCarriageTravel.h>

#include <Structs/CarriageTravel.h>
#include <Structs/PlayerRevive.h>
#include <Structs/ServerSettings.h>

#include <PlayerCharacter.h>
#include <Games/ActorExtension.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESGlobal.h>
#include <Forms/BGSKeyword.h>
#include <Forms/TESWorldSpace.h>
#include <Components/BGSKeywordForm.h>
#include <Games/Overrides.h>
#include <Games/References.h>
#include <Games/TES.h>
#include <AI/AIProcess.h>
#include <AI/Movement/PlayerControls.h>
#include <EquipManager.h>
#include <Forms/TESRace.h>
#include <Interface/UI.h>
#include <Services/OverlayService.h>

PlayerService::PlayerService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_dispatcher(aDispatcher)
    , m_transport(aTransport)
{
    m_updateConnection = m_dispatcher.sink<UpdateEvent>().connect<&PlayerService::OnUpdate>(this);
    m_connectedConnection = m_dispatcher.sink<ConnectedEvent>().connect<&PlayerService::OnConnected>(this);
    m_disconnectedConnection = m_dispatcher.sink<DisconnectedEvent>().connect<&PlayerService::OnDisconnected>(this);
    m_settingsConnection = m_dispatcher.sink<ServerSettings>().connect<&PlayerService::OnServerSettingsReceived>(this);
    m_notifyRespawnConnection = m_dispatcher.sink<NotifyPlayerRespawn>().connect<&PlayerService::OnNotifyPlayerRespawn>(this);
    m_notifyDownedConnection = m_dispatcher.sink<NotifyPlayerDowned>().connect<&PlayerService::OnNotifyPlayerDowned>(this);
    m_reviveConnection = m_dispatcher.sink<RevivePlayerEvent>().connect<&PlayerService::OnRevivePlayer>(this);
    m_gridCellChangeConnection = m_dispatcher.sink<GridCellChangeEvent>().connect<&PlayerService::OnGridCellChangeEvent>(this);
    m_cellChangeConnection = m_dispatcher.sink<CellChangeEvent>().connect<&PlayerService::OnCellChangeEvent>(this);
    m_playerLevelConnection = m_dispatcher.sink<PlayerLevelEvent>().connect<&PlayerService::OnPlayerLevelEvent>(this);
    m_partyJoinedConnection = aDispatcher.sink<PartyJoinedEvent>().connect<&PlayerService::OnPartyJoinedEvent>(this);
    m_partyLeftConnection = aDispatcher.sink<PartyLeftEvent>().connect<&PlayerService::OnPartyLeftEvent>(this);
    m_carriageActivateConnection = aDispatcher.sink<ActivateEvent>().connect<&PlayerService::OnActivate>(this);
    m_teleportConnection = aDispatcher.sink<NotifyTeleport>().connect<&PlayerService::OnNotifyTeleport>(this);
    EventDispatcherManager::Get()->fastTravelEndEvent.RegisterSink(this);
}

void PlayerService::OnUpdate(const UpdateEvent&) noexcept
{
    RunRespawnUpdates();
    RunPostDeathUpdates();
    RunRevivePrompt();
    RunDifficultyUpdates();
    RunLevelUpdates();
    RunBeastFormDetection();
    RunCarriageTravelUpdates();
}

void PlayerService::OnConnected(const ConnectedEvent& acEvent) noexcept
{
    // TODO: SkyrimTogether.esm
    TESGlobal* pKillMove = Cast<TESGlobal>(TESForm::GetById(0x100F19));
    pKillMove->f = 0.f;

    TESGlobal* pWorldEncountersEnabled = Cast<TESGlobal>(TESForm::GetById(0xB8EC1));
    pWorldEncountersEnabled->f = 0.f;
}

void PlayerService::OnDisconnected(const DisconnectedEvent& acEvent) noexcept
{
    m_carriage.reset();
    m_carriageTeleport.reset();
    m_reportedCell = {};
    PlayerCharacter::Get()->SetDifficulty(m_previousDifficulty);
    m_serverDifficulty = m_previousDifficulty = 6;

    ToggleDeathSystem(false);

    TESGlobal* pKillMove = Cast<TESGlobal>(TESForm::GetById(0x100F19));
    pKillMove->f = 1.f;

    // Restore to the default value (150 in skyrim, 175 in fallout 4)
    float* greetDistance = Settings::GetGreetDistance();
    *greetDistance = 150.f;

    TESGlobal* pWorldEncountersEnabled = Cast<TESGlobal>(TESForm::GetById(0xB8EC1));
    pWorldEncountersEnabled->f = 1.f;
}

void PlayerService::OnServerSettingsReceived(const ServerSettings& acSettings) noexcept
{
    m_previousDifficulty = *Settings::GetDifficulty();
    PlayerCharacter::Get()->SetDifficulty(acSettings.Difficulty);
    m_serverDifficulty = acSettings.Difficulty;

    if (!acSettings.GreetingsEnabled)
    {
        float* greetDistance = Settings::GetGreetDistance();
        *greetDistance = 0.f;
    }

    ToggleDeathSystem(acSettings.DeathSystemEnabled);
    m_respawnDelay = acSettings.PlayerRespawnDelay;
}

void PlayerService::OnNotifyPlayerRespawn(const NotifyPlayerRespawn& acMessage) noexcept
{
    const bool cCurrentEpisode = m_localDowned && m_localDowned->Id == acMessage.DownId;
    if (!m_isDeathSystemEnabled || !acMessage.DownId || acMessage.DownId == m_grantedDownId ||
        (!cCurrentEpisode && acMessage.DownId != m_settlingDownId))
        return;

    auto* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    m_grantedDownId = acMessage.DownId;
    if (acMessage.GoldLost > 0)
    {
        pPlayer->PayGold(acMessage.GoldLost);
        Utils::ShowHudMessage(String(fmt::format("You lost {} gold.", acMessage.GoldLost)));
    }
    if (!cCurrentEpisode || !pPlayer->actorState.IsBleedingOut())
        return;

    const auto now = std::chrono::steady_clock::now();
    m_localDowned->Resolved = true;
    m_localDowned->NextAttempt = now + 1s;
    if (acMessage.Revived)
    {
        pPlayer->RecoverFromBleedout();
        Utils::ShowHudMessage("Another player revived you.");
    }
    else
    {
        FadeOutGame(true, true, 0.5f, true, 2.f);
        pPlayer->PayCrimeGoldToAllFactions();
        pPlayer->RespawnPlayer();
    }

    auto* pEquipManager = EquipManager::Get();
    if (TESForm* pSpell = TESForm::GetById(m_cachedMainSpellId))
        pEquipManager->EquipSpell(pPlayer, pSpell, 0);
    if (TESForm* pSpell = TESForm::GetById(m_cachedSecondarySpellId))
        pEquipManager->EquipSpell(pPlayer, pSpell, 1);
    if (TESForm* pShout = TESForm::GetById(m_cachedPowerId))
        pEquipManager->EquipShout(pPlayer, pShout);

    m_knockdownDeadline = now + 1500ms;
    m_knockdownStart = true;
    m_knockdownOnlyIfDown = acMessage.Revived;
}

void PlayerService::OnNotifyPlayerDowned(const NotifyPlayerDowned& acMessage) noexcept
{
    if (acMessage.DownId)
        m_downedPlayers.insert_or_assign(acMessage.ActorId, acMessage.DownId);
    else
        m_downedPlayers.erase(acMessage.ActorId);
}

std::optional<PlayerService::ReviveTarget> PlayerService::FindNearbyReviveTarget() const noexcept
{
    if (!m_isDeathSystemEnabled || !m_transport.IsOnline() || m_localDowned || m_downedPlayers.empty() ||
        m_world.GetOverlayService().GetActive())
        return std::nullopt;

    const auto* pUI = UI::Get();
    static const BSFixedString s_loadingMenu("Loading Menu");
    static const BSFixedString s_dialogueMenu("Dialogue Menu");
    if (!pUI || pUI->numPausesGame || pUI->IsUnpausedMenuOpen() ||
        pUI->GetMenuOpen(s_loadingMenu) || pUI->GetMenuOpen(s_dialogueMenu))
        return std::nullopt;

    auto* pPlayer = PlayerCharacter::Get();
    const auto* pCell = pPlayer ? pPlayer->GetParentCellEx() : nullptr;
    if (!pCell || pPlayer->actorState.IsBleedingOut())
        return std::nullopt;

    std::optional<ReviveTarget> target;
    float squaredDistance = PlayerRevive::kDistance * PlayerRevive::kDistance;
    const auto view = m_world.view<RemoteComponent, FormIdComponent, PlayerComponent>();
    for (const auto entity : view)
    {
        const uint32_t cActorId = view.get<RemoteComponent>(entity).Id;
        const auto downed = m_downedPlayers.find(cActorId);
        if (downed == m_downedPlayers.end())
            continue;

        auto* pActor = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(entity).Id));
        if (!pActor || !pActor->GetExtension()->IsRemotePlayer() || pActor->GetParentCellEx() != pCell || !pActor->GetNiNode())
            continue;

        const auto delta = glm::vec3(pActor->position) - glm::vec3(pPlayer->position);
        const float cDistance = glm::dot(delta, delta);
        if (cDistance <= squaredDistance)
        {
            squaredDistance = cDistance;
            target = ReviveTarget{cActorId, downed->second};
        }
    }
    return target;
}

void PlayerService::OnRevivePlayer(const RevivePlayerEvent&) noexcept
{
    if (const auto target = FindNearbyReviveTarget())
    {
        PlayerRespawnRequest request;
        request.Action = PlayerRespawnRequest::Revive;
        request.DownId = target->DownId;
        request.TargetId = target->ActorId;
        m_transport.Send(request);
    }
}

void PlayerService::RunRevivePrompt() noexcept
{
    const auto now = std::chrono::steady_clock::now();
    if (now < m_nextRevivePrompt)
        return;

    m_nextRevivePrompt = now + 500ms;
    auto target = FindNearbyReviveTarget();
    if (target && target != m_promptTarget)
        Utils::ShowHudMessage("Press F10 to revive the nearby player.");
    m_promptTarget = target;
}

void PlayerService::OnGridCellChangeEvent(const GridCellChangeEvent& acEvent) const noexcept
{
    uint32_t baseId = 0;
    uint32_t modId = 0;

    if (m_world.GetModSystem().GetServerModId(acEvent.WorldSpaceId, modId, baseId))
    {
        ShiftGridCellRequest request;
        request.WorldSpaceId = GameId(modId, baseId);
        request.PlayerCell = acEvent.PlayerCell;
        request.CenterCoords = acEvent.CenterCoords;
        request.Cells = acEvent.Cells;

        m_transport.Send(request);
    }
}

void PlayerService::OnCellChangeEvent(const CellChangeEvent& acEvent) noexcept
{
    if (acEvent.WorldSpaceId)
    {
        EnterExteriorCellRequest message;
        message.CellId = acEvent.CellId;
        message.WorldSpaceId = acEvent.WorldSpaceId;
        message.CurrentCoords = acEvent.CurrentCoords;

        if (m_transport.Send(message))
            m_reportedCell = message.CellId;
    }
    else
    {
        EnterInteriorCellRequest message;
        message.CellId = acEvent.CellId;

        if (m_transport.Send(message))
            m_reportedCell = message.CellId;
    }
}

void PlayerService::OnActivate(const ActivateEvent& acEvent) noexcept
{
    auto* pPlayer = PlayerCharacter::Get();
    if (!m_transport.IsOnline() || !pPlayer || acEvent.pActivator != pPlayer || !acEvent.pObject ||
        !acEvent.pObject->baseForm || acEvent.pObject->baseForm->formType != FormType::Furniture)
        return;

    // Skyrim.esm's isCartTravelPlayer keyword marks the actual passenger furniture.
    constexpr uint32_t kCarriageSeatKeyword = 0x103444;
    const auto* pSkyrim = ModManager::Get()->GetByName("Skyrim.esm");
    auto* pKeyword = pSkyrim ? Cast<BGSKeyword>(TESForm::GetById((static_cast<uint32_t>(pSkyrim->GetId()) << 24) | kCarriageSeatKeyword)) : nullptr;
    const auto* pKeywords = Cast<BGSKeywordForm>(acEvent.pObject->baseForm);
    const auto* pCell = acEvent.pObject->GetParentCellEx();
    if (!pKeyword || !pKeywords || !pKeywords->Contains(pKeyword) || !pCell || pCell != pPlayer->GetParentCellEx())
        return;

    RequestCarriageTravel request;
    const auto& mods = m_world.GetModSystem();
    if (acEvent.pObject->IsTemporary() || !mods.GetServerModId(acEvent.pObject->formID, request.Seat) ||
        mods.GetGameId(request.Seat) != acEvent.pObject->formID ||
        !mods.GetServerModId(pCell->formID, request.Cell) || request.Cell != m_reportedCell)
        return;

    if (++m_nextCarriageToken == 0)
        ++m_nextCarriageToken;
    request.Token = m_nextCarriageToken;
    request.Position = acEvent.pObject->position;
    if (m_transport.Send(request))
    {
        GameId worldSpace{};
        if (const auto* pWorldSpace = pPlayer->GetWorldSpace())
            mods.GetServerModId(pWorldSpace->formID, worldSpace);
        m_carriageTeleport.reset();
        m_carriage = CarriageBoarding{request.Seat, request.Cell, worldSpace, request.Position,
            std::chrono::steady_clock::now() + CarriageTravel::kBoardingLifetime, request.Token};
    }
}

BSTEventResult PlayerService::OnEvent(const TESFastTravelEndEvent*, const EventDispatcher<TESFastTravelEndEvent>*)
{
    m_world.GetRunner().Queue([this]()
    {
        if (m_carriage)
            m_carriage->Arrived = true;
    });
    return BSTEventResult::kOk;
}

void PlayerService::OnNotifyTeleport(const NotifyTeleport& acMessage) noexcept
{
    if (!acMessage.CarriageToken)
    {
        m_carriage.reset();
        m_carriageTeleport.reset();
        ApplyTeleport(acMessage);
    }
    else if (m_carriage && m_carriage->Token == acMessage.CarriageToken)
        m_carriageTeleport = acMessage;
}

void PlayerService::RunCarriageTravelUpdates() noexcept
{
    if (!m_carriage)
        return;
    const auto* pUI = UI::Get();
    static const BSFixedString s_mapMenu("MapMenu");
    static const BSFixedString s_loadingMenu("Loading Menu");
    if (std::chrono::steady_clock::now() >= m_carriage->ExpiresAt || (pUI && pUI->GetMenuOpen(s_mapMenu)))
    {
        m_carriage.reset();
        m_carriageTeleport.reset();
        return;
    }
    if (pUI && pUI->GetMenuOpen(s_loadingMenu))
    {
        m_carriage->SawLoading |= m_carriage->SawRidingControls;
        return;
    }

    auto* pPlayer = PlayerCharacter::Get();
    const auto* pCell = pPlayer ? pPlayer->GetParentCellEx() : nullptr;
    GameId currentCell{};
    if (!pCell || !m_world.GetModSystem().GetServerModId(pCell->formID, currentCell))
        return;

    if (currentCell == m_carriage->Origin)
    {
        const glm::vec3 delta = glm::vec3(pPlayer->position) - m_carriage->Position;
        m_carriage->SawRidingControls |= !BSInputEnableManager::IsMovementControlsEnabled();
        if (glm::dot(delta, delta) > CarriageTravel::kBoardingDistance * CarriageTravel::kBoardingDistance)
        {
            m_carriage.reset();
            m_carriageTeleport.reset();
        }
        else if (m_carriageTeleport && !m_carriage->Arrived)
        {
            const auto teleport = *m_carriageTeleport;
            m_carriage.reset();
            m_carriageTeleport.reset();
            ApplyTeleport(teleport);
        }
        return;
    }
    if (currentCell != m_reportedCell)
        return;

    // Some scripted travel paths don't produce the native completion event. Vanilla
    // carriage scripts disable movement before fading into a distant exterior load.
    if (!m_carriage->Arrived && m_carriage->SawLoading && m_carriage->WorldSpace)
    {
        GameId worldSpace{};
        const auto* pWorldSpace = pPlayer->GetWorldSpace();
        const glm::vec3 delta = glm::vec3(pPlayer->position) - m_carriage->Position;
        constexpr float kLongTravelDistance = 4096.f;
        m_carriage->Arrived = pWorldSpace && m_world.GetModSystem().GetServerModId(pWorldSpace->formID, worldSpace) &&
            worldSpace == m_carriage->WorldSpace && glm::dot(delta, delta) >= kLongTravelDistance * kLongTravelDistance;
    }
    if (!m_carriage->Arrived)
        return;

    RequestCarriageTravel request;
    request.Seat = m_carriage->Seat;
    request.Token = m_carriage->Token;
    request.Cell = currentCell;
    request.Position = pPlayer->position;
    request.Arrived = true;
    if (m_transport.Send(request))
    {
        m_carriage.reset();
        m_carriageTeleport.reset();
    }
}

void PlayerService::ApplyTeleport(const NotifyTeleport& acMessage) noexcept
{
    auto& modSystem = m_world.GetModSystem();

    const uint32_t cellId = modSystem.GetGameId(acMessage.CellId);
    TESObjectCELL* pCell = Cast<TESObjectCELL>(TESForm::GetById(cellId));
    if (!pCell)
    {
        const uint32_t worldSpaceId = modSystem.GetGameId(acMessage.WorldSpaceId);
        TESWorldSpace* pWorldSpace = Cast<TESWorldSpace>(TESForm::GetById(worldSpaceId));
        if (pWorldSpace)
        {
            GridCellCoords coordinates = GridCellCoords::CalculateGridCellCoords(acMessage.Position);
            pCell = pWorldSpace->LoadCell(coordinates.X, coordinates.Y);
        }

        if (!pCell)
        {
            spdlog::error("Failed to fetch cell to teleport to.");
            m_world.GetOverlayService().SendSystemMessage("Teleporting to player failed.");
            return;
        }
    }

    PlayerCharacter::Get()->MoveTo(pCell, acMessage.Position);
}

void PlayerService::OnPlayerLevelEvent(const PlayerLevelEvent& acEvent) const noexcept
{
    if (!m_transport.IsConnected())
        return;

    PlayerLevelRequest request{};
    request.NewLevel = PlayerCharacter::Get()->GetLevel();

    m_transport.Send(request);
}

void PlayerService::OnPartyJoinedEvent(const PartyJoinedEvent& acEvent) noexcept
{
    // TODO: this can be done a bit prettier
    if (acEvent.IsLeader)
    {
        TESGlobal* pWorldEncountersEnabled = Cast<TESGlobal>(TESForm::GetById(0xB8EC1));
        pWorldEncountersEnabled->f = 1.f;
    }
}

void PlayerService::OnPartyLeftEvent(const PartyLeftEvent& acEvent) noexcept
{
    // TODO: this can be done a bit prettier
    if (World::Get().GetTransport().IsConnected())
    {
        TESGlobal* pWorldEncountersEnabled = Cast<TESGlobal>(TESForm::GetById(0xB8EC1));
        pWorldEncountersEnabled->f = 0.f;
    }
}

void PlayerService::RunRespawnUpdates() noexcept
{
    auto* pPlayer = PlayerCharacter::Get();
    if (!m_isDeathSystemEnabled || !m_transport.IsOnline() || !pPlayer)
        return;

    if (!pPlayer->actorState.IsBleedingOut())
    {
        if (m_localDowned)
        {
            if (m_localDowned->Reported)
            {
                PlayerRespawnRequest recovered;
                recovered.Action = PlayerRespawnRequest::Recovered;
                recovered.DownId = m_localDowned->Id;
                if (!m_transport.Send(recovered))
                    return;
                // A grant already in flight may still carry a server-committed penalty.
                m_settlingDownId = recovered.DownId;
            }
            pPlayer->SetNoBleedoutRecovery(true);
            m_localDowned.reset();
        }

        m_cachedMainSpellId = pPlayer->magicItems[0] ? pPlayer->magicItems[0]->formID : 0;
        m_cachedSecondarySpellId = pPlayer->magicItems[1] ? pPlayer->magicItems[1]->formID : 0;
        m_cachedPowerId = pPlayer->equippedShout ? pPlayer->equippedShout->formID : 0;
        return;
    }

    if (!m_localDowned)
    {
        if (++m_nextDownId == 0)
            ++m_nextDownId;
        m_localDowned = LocalDownedState{m_nextDownId};
        // Some bleedout paths leave positive health. The existing player recovery
        // sequence expects to restore health from zero.
        if (pPlayer->GetActorValue(ActorValueInfo::kHealth) > 0.f)
            pPlayer->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, 0.f);
    }

    auto& downed = *m_localDowned;
    const auto now = std::chrono::steady_clock::now();
    PlayerRespawnRequest request;
    request.DownId = downed.Id;
    if (!downed.Reported)
    {
        if (!Utils::GetLocalOwnershipToken(pPlayer->formID) || !m_transport.Send(request))
            return;
        downed.Reported = true;
        downed.NextAttempt = now + std::chrono::seconds(m_respawnDelay);
        Utils::ShowHudMessage(String(fmt::format("You are down. A nearby player can revive you before you respawn in {} seconds.", m_respawnDelay)));
        return;
    }
    if (now < downed.NextAttempt)
        return;

    if (downed.Resolved)
    {
        // Keep the existing bleedout recovery enabled until Skyrim leaves this
        // state. Retrying must not charge gold or teleport the player again.
        pPlayer->SetNoBleedoutRecovery(false);
        if (pPlayer->GetActorValue(ActorValueInfo::kHealth) <= 0.f)
            pPlayer->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, 1000000);
        downed.NextAttempt = now + 1s;
        return;
    }

    request.Action = PlayerRespawnRequest::Respawn;
    // The server may have received the downed report later than this client sent it.
    // Retry while still down and let the server decide which request resolves it.
    downed.NextAttempt = now + 500ms;
    m_transport.Send(request);
}

void PlayerService::RunPostDeathUpdates() noexcept
{
    if (!m_isDeathSystemEnabled)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (m_knockdownStart && now >= m_knockdownDeadline)
    {
        auto* pPlayer = PlayerCharacter::Get();
        if (!pPlayer || (m_knockdownOnlyIfDown && !pPlayer->actorState.IsBleedingOut()))
            m_knockdownStart = false;
        else if (pPlayer->currentProcess)
        {
            // Preserve the existing workaround for a player stuck in ragdoll.
            PlayerCharacter::SetGodMode(true);
            m_godmodeStart = true;
            pPlayer->currentProcess->KnockExplosion(pPlayer, &pPlayer->position, 0.f);
            if (!m_knockdownOnlyIfDown)
                FadeOutGame(false, true, 0.5f, true, 2.f);

            m_godmodeDeadline = now + 10s;
            m_knockdownStart = false;
        }
    }

    if (m_godmodeStart && now >= m_godmodeDeadline)
    {
        PlayerCharacter::SetGodMode(false);
        m_godmodeStart = false;
    }
}

void PlayerService::RunDifficultyUpdates() const noexcept
{
    if (!m_transport.IsConnected())
        return;

    PlayerCharacter::Get()->SetDifficulty(m_serverDifficulty);
}

void PlayerService::RunLevelUpdates() const noexcept
{
    // The LevelUp hook is kinda weird, so ehh, just check periodically, doesn't really cost anything.

    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 1000ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    static uint16_t oldLevel = PlayerCharacter::Get()->GetLevel();

    uint16_t newLevel = PlayerCharacter::Get()->GetLevel();
    if (newLevel != oldLevel)
    {
        PlayerLevelRequest request{};
        request.NewLevel = newLevel;

        m_transport.Send(request);

        oldLevel = newLevel;
    }
}

void PlayerService::RunBeastFormDetection() const noexcept
{
    static uint32_t lastRaceFormID = 0;
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 250ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer->race)
        return;

    if (pPlayer->race->formID == lastRaceFormID)
        return;

    if (pPlayer->race->formID == 0x200283A || pPlayer->race->formID == 0xCDD84)
        m_world.GetDispatcher().trigger(BeastFormChangeEvent());

    lastRaceFormID = pPlayer->race->formID;
}

void PlayerService::ToggleDeathSystem(bool aSet) noexcept
{
    m_isDeathSystemEnabled = aSet;

    if (!aSet)
    {
        if (m_localDowned && m_localDowned->Reported && m_transport.IsOnline())
        {
            PlayerRespawnRequest recovered;
            recovered.Action = PlayerRespawnRequest::Recovered;
            recovered.DownId = m_localDowned->Id;
            m_transport.Send(recovered);
        }
        m_localDowned.reset();
        m_downedPlayers.clear();
        m_promptTarget.reset();
        m_nextRevivePrompt = {};
        m_settlingDownId = 0;
        m_grantedDownId = 0;
        if (m_knockdownStart && !m_knockdownOnlyIfDown)
            FadeOutGame(false, true, 0.5f, true, 2.f);
        m_knockdownStart = false;
        if (m_godmodeStart)
        {
            PlayerCharacter::SetGodMode(false);
            m_godmodeStart = false;
        }
    }

    if (auto* pPlayer = PlayerCharacter::Get())
        pPlayer->SetPlayerRespawnMode(aSet);
}
