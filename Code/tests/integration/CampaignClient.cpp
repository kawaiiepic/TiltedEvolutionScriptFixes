// The transport assertions must run when testing a release server.
#undef NDEBUG
#include <cassert>

#include <EncodingPch.h>
#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>
#include <TiltedCore/ViewBuffer.hpp>
#include <Client.hpp>
#include <Packet.hpp>
#include <steam/isteamnetworkingutils.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <optional>
#include <thread>
#include <utility>

using namespace TiltedPhoques;
using namespace std::chrono_literals;

class CampaignClient final : public Client
{
public:
    CampaignClient(const char* aMode, const char* aVersion)
        : Mode(aMode), Version(aVersion)
    {
    }

    void OnUpdate() override
    {
        const auto now = std::chrono::steady_clock::now();
        if (DeathRequestAt && now >= *DeathRequestAt)
        {
            DeathRequestAt.reset();
            if (Mode == "death-helper")
            {
                SendDeathRequest(PlayerRespawnRequest::Revive, kReviveDownId, DownedActorId);
                SendDeathRequest(PlayerRespawnRequest::Revive, kReviveDownId, DownedActorId);
            }
            else if (CurrentDownId == kReviveDownId)
                ReportDowned(kRespawnDownId);
            else
            {
                assert(CurrentDownId == kRespawnDownId);
                SendDeathRequest(PlayerRespawnRequest::Respawn, kRespawnDownId);
                SendDeathRequest(PlayerRespawnRequest::Respawn, kRespawnDownId);
            }
        }
        if (DepartureAt && now >= *DepartureAt)
        {
            DepartureAt.reset();
            EnterExteriorCellRequest cell{};
            cell.CellId = GameId(VanillaMod, 0xA22);
            cell.WorldSpaceId = GameId(VanillaMod, 0x3C);
            Transmit(cell);

            RequestCarriageTravel arrival{};
            arrival.Seat = GameId(QuestMod, 0x915);
            arrival.Cell = cell.CellId;
            arrival.Arrived = true;
            arrival.Token = 99; // An earlier trip cannot pull the passengers along.
            arrival.Position = glm::vec3(100.f, 0.f, 0.f);
            Transmit(arrival);
            arrival.Token = 1;
            arrival.Position = glm::vec3(32.f, 64.f, 128.f);
            Transmit(arrival);
            Transmit(arrival); // Duplicate completion should not send a second teleport.
        }
        if (OwnerReady && now - ReadyAt > 4s)
            Done = true;
    }
    void OnDisconnected(EDisconnectReason aReason) override
    {
        std::fprintf(stderr, "transport disconnected (%d)\n", static_cast<int>(aReason));
    }

    void OnConnected() override
    {
        std::fprintf(stderr, "transport connected for %s\n", Mode.c_str());
        AuthenticationRequest request{};
        request.Phase = AuthenticationRequest::Login;
        request.Version = Version.c_str();
        Transmit(request);
    }

    void EnterWorld()
    {
        AuthenticationRequest request{};
        request.Phase = AuthenticationRequest::EnterWorld;
        request.Version = Version.c_str();
        request.Username = Mode.c_str();
        request.Level = 1;
        request.CellId = GameId(9, 0xA21);
        if (Mode == "restarted")
            request.UserMods.ModList.push_back({"Added First.esp", 0, false});
        request.UserMods.ModList.push_back({"Skyrim.esm", 9, false});
        if (Mode != "missing")
            request.UserMods.ModList.push_back({"Quest Pack.esl", 0, true});
        Transmit(request);
    }

    void OnConsume(const void* apData, uint32_t aSize) override
    {
        ViewBuffer buffer(const_cast<uint8_t*>(static_cast<const uint8_t*>(apData)), aSize);
        Buffer::Reader reader(&buffer);
        auto message = ServerMessageFactory{}.Extract(reader);
        assert(message);
        if (message->GetOpcode() == AuthenticationResponse::Opcode)
        {
            const auto reply = CastUnique<AuthenticationResponse>(std::move(message));
            if (Mode == "wrong")
            {
                assert(reply->Type == AuthenticationResponse::ResponseType::kWrongVersion);
                Done = true;
                return;
            }
            if (reply->Type == AuthenticationResponse::ResponseType::kLobbyAccepted)
            {
                assert(!Authed);
                assert(reply->PlayerId == 0);
                EnterWorld();
                return;
            }
            assert(reply->Type == AuthenticationResponse::ResponseType::kAccepted);
            assert(!reply->Settings.ShareCombatExperience);
            if (Mode.starts_with("death-"))
                assert(reply->Settings.DeathSystemEnabled && reply->Settings.PlayerRespawnDelay == 1);
            Authed = true;
            PlayerId = reply->PlayerId;
            for (const auto& mod : reply->UserMods.ModList)
            {
                if (mod.Filename == "Quest Pack.esl")
                    QuestMod = mod.Id;
                if (mod.Filename == "Skyrim.esm")
                    VanillaMod = mod.Id;
            }
            if (Mode == "write")
            {
                SendEvent(RequestQuestUpdate::Started, 0);
                for (uint16_t stage = 1; stage <= 20; ++stage)
                    SendEvent(RequestQuestUpdate::StageUpdate, stage);
                SendEvent(RequestQuestUpdate::StageUpdate, 19);
                SendEvent(RequestQuestUpdate::Stopped, 19);
                SendEvent(RequestQuestUpdate::Stopped, 40);
                SendEvent(RequestQuestUpdate::Completed, 40);
                SendEvent(RequestQuestUpdate::Completed, 40);
                SendEvent(RequestQuestUpdate::Started, 1);
                SendEvent(RequestQuestUpdate::StageUpdate, 80, 0x8AC);
                EnterInteriorCellRequest cell{};
                cell.CellId = GameId(0, 0xA21);
                Transmit(cell);
                AssignCharacterRequest character{};
                character.ReferenceId = GameId(0, 0x14);
                character.CellId = GameId(0, 0xA21);
                Transmit(character);
                AssignNpc();
                AssignNpc(0x89A, false);
                AssignNpc(0x89B);
                AssignNpc(0x89C, true, true);
            }
            if (Mode == "resurrect" || Mode == "verify-npc")
                AssignNpc();
            if (Mode.starts_with("weather"))
            {
                EnterExteriorCellRequest cell{};
                cell.CellId = GameId(VanillaMod, 0x510);
                cell.WorldSpaceId = GameId(VanillaMod, 0x3C);
                Transmit(cell);
                AssignCharacterRequest character{};
                character.ReferenceId = GameId(0, 0x14);
                character.CellId = cell.CellId;
                character.WorldSpaceId = cell.WorldSpaceId;
                Transmit(character);
            }
            if (Mode.starts_with("stock-"))
            {
                EnterInteriorCellRequest cell{};
                cell.CellId = GameId(VanillaMod, 0xA21);
                Transmit(cell);
                AssignLock();
            }
            if (Mode.starts_with("animation-") || Mode.starts_with("carriage-") || Mode.starts_with("death-"))
            {
                EnterInteriorCellRequest cell{};
                cell.CellId = GameId(VanillaMod, 0xA21);
                Transmit(cell);
                AssignCharacterRequest character{};
                character.ReferenceId = GameId(0, 0x14);
                character.CellId = cell.CellId;
                if (Mode.starts_with("death-"))
                {
                    character.Position = glm::vec3(Mode == "death-distant" ? 1024.f : 0.f, 0.f, 0.f);
                    if (Mode == "death-patient")
                    {
                        auto& gold = character.CurrentActorData.InitialInventory.Entries.emplace_back();
                        gold.BaseId = GameId(VanillaMod, 0xF);
                        gold.Count = 100;
                    }
                }
                Transmit(character);
            }
            return;
        }

        if (Mode.starts_with("death-") && ConsumeDeathPacket(message))
            return;

        if (message->GetOpcode() == NotifyTeleport::Opcode && Mode.starts_with("carriage-"))
        {
            assert(Mode == "carriage-rider" && !SawCarriageTeleport);
            const auto teleport = CastUnique<NotifyTeleport>(std::move(message));
            assert(teleport->CarriageToken == 2 && teleport->CellId == GameId(VanillaMod, 0xA22));
            assert(teleport->WorldSpaceId == GameId(VanillaMod, 0x3C));
            assert(static_cast<const glm::vec3&>(teleport->Position) == glm::vec3(32.f, 64.f, 128.f));
            SawCarriageTeleport = true;
            OwnerReady = true;
            ReadyAt = std::chrono::steady_clock::now() - 3s;
            return;
        }

        if (message->GetOpcode() == NotifyScriptAnimation::Opcode)
        {
            assert(Mode == "animation-receiver");
            const auto animation = CastUnique<NotifyScriptAnimation>(std::move(message));
            assert(animation->Id == GameId(QuestMod, 0x912));
            assert(animation->Cell == GameId(VanillaMod, 0xA21));
            const auto& [name, tick] = kObjectAnimations.at(AnimationCount++);
            assert(animation->EventName == name && animation->Tick == tick);
            if (AnimationCount == kObjectAnimations.size())
                Done = true;
            return;
        }

        if (message->GetOpcode() == NotifyWeatherChange::Opcode && Mode.starts_with("weather"))
        {
            const auto state = CastUnique<NotifyWeatherChange>(std::move(message));
            assert(state->Cell == GameId(VanillaMod, 0x510));
            if (Mode == "weather-owner")
            {
                assert(state->Owner == PlayerId);
                if (!state->Id)
                    SendWeather(0x101);
                else if (!OwnerReady)
                {
                    assert(state->Id == GameId(QuestMod, 0x101));
                    std::puts("WEATHER_OWNER_READY");
                    std::fflush(stdout);
                    OwnerReady = true;
                    ReadyAt = std::chrono::steady_clock::now();
                }
            }
            else if (state->Owner != PlayerId)
            {
                assert(state->Id == GameId(QuestMod, 0x101));
                SawOwner = true;
                SendWeather(0x103); // Reports from the follower must be ignored.
            }
            else
            {
                assert(SawOwner);
                if (state->Id == GameId(QuestMod, 0x101))
                    SendWeather(0x102);
                else
                {
                    assert(state->Id == GameId(QuestMod, 0x102));
                    Done = true;
                }
            }
            return;
        }

        if (message->GetOpcode() == AssignCharacterResponse::Opcode)
        {
            const auto assignment = CastUnique<AssignCharacterResponse>(std::move(message));
            if (assignment->Cookie >= 0x899 && assignment->Cookie <= 0x89C)
            {
                assert(assignment->Owner);
                if (assignment->Cookie != 0x899)
                {
                    assert(!assignment->HasCampaignDeath);
                    RequestDeathStateChange life{};
                    life.Id = assignment->ServerId;
                    life.OwnershipEpoch = assignment->OwnershipEpoch;
                    life.IsDead = true;
                    Transmit(life);
                    return;
                }
                if (Mode == "verify-npc")
                {
                    assert(assignment->HasCampaignDeath && !assignment->IsDead);
                    Done = true;
                    return;
                }
                assert(Mode == "write" ? !assignment->HasCampaignDeath : assignment->HasCampaignDeath && assignment->IsDead);
                RequestDeathStateChange life{};
                life.Id = assignment->ServerId;
                life.OwnershipEpoch = assignment->OwnershipEpoch;
                life.IsDead = Mode == "write";
                Transmit(life);
                SawNpc = true;
            }
            else if (Mode == "write")
                AssignLock();
            else if (Mode == "animation-receiver")
            {
                std::puts("OBJECT_ANIMATION_READY");
                std::fflush(stdout);
            }
            else if (Mode == "animation-sender")
            {
                ScriptAnimationRequest animation{};
                animation.Id = GameId(QuestMod, 0x912);
                animation.Cell = GameId(VanillaMod, 0xA21);
                Transmit(animation); // Missing event name.
                animation.Cell = GameId(VanillaMod, 0xA22);
                animation.EventName = "WrongCell";
                Transmit(animation);
                animation.Cell = GameId(VanillaMod, 0xA21);
                for (const auto& [name, tick] : kObjectAnimations)
                {
                    animation.EventName = name;
                    animation.Tick = tick;
                    Transmit(animation);
                }
                OwnerReady = true;
                ReadyAt = std::chrono::steady_clock::now();
            }
            else if (Mode.starts_with("carriage-"))
            {
                if (Mode != "carriage-bystander")
                {
                    RequestCarriageTravel boarding{};
                    boarding.Seat = GameId(QuestMod, Mode == "carriage-other" ? 0x916 : 0x915);
                    boarding.Cell = GameId(VanillaMod, 0xA21);
                    boarding.Position = glm::vec3(0.f);
                    boarding.Token = Mode == "carriage-leader" ? 1 : 2;
                    Transmit(boarding);
                }
                if (Mode == "carriage-leader")
                    DepartureAt = std::chrono::steady_clock::now() + 400ms;
                else
                {
                    std::puts("CARRIAGE_READY");
                    std::fflush(stdout);
                }
                if (Mode != "carriage-rider")
                {
                    OwnerReady = true;
                    ReadyAt = std::chrono::steady_clock::now() + 2s;
                }
            }
            return;
        }
        if (message->GetOpcode() == AssignObjectsResponse::Opcode)
        {
            const auto response = CastUnique<AssignObjectsResponse>(std::move(message));
            assert(response->Objects.size() == 1);
            const auto& object = response->Objects.front();
            assert(object.IsSenderFirst == (Mode != "stock-follower"));
            if (Mode == "write")
            {
                assert(!object.HasCampaignLock);
                LockChangeRequest change{};
                change.CellId = GameId(0, 0xA21);
                change.Id = GameId(QuestMod, 0x8AD);
                change.IsLocked = false;
                change.LockLevel = 0;
                Transmit(change);
                RequestInventoryChanges stock{};
                stock.ServerId = object.ServerId;
                stock.Item.BaseId = GameId(VanillaMod, 0xF);
                stock.Item.Count = -4;
                Transmit(stock);
            }
            else
            {
                assert(Mode.starts_with("stock-"));
                assert(object.HasCampaignLock == (Mode == "stock-owner"));
                assert(!object.CurrentLockData.IsLocked && object.CurrentLockData.LockLevel == 0);
                assert(object.HasCampaignStock);
                assert(object.CurrentInventory.Entries.size() == 1);
                assert(object.CurrentInventory.Entries.front().Count == 5);
                if (Mode == "stock-owner")
                {
                    std::puts("STOCK_OWNER_READY");
                    std::fflush(stdout);
                    OwnerReady = true;
                    ReadyAt = std::chrono::steady_clock::now();
                }
                else
                    Done = true;
            }
            return;
        }

        if (Mode.starts_with("stock-") || Mode == "resurrect" || Mode == "verify-npc" ||
            Mode.starts_with("weather") || Mode.starts_with("animation-") || Mode.starts_with("carriage-") ||
            Mode.starts_with("death-"))
        {
            if (message->GetOpcode() == NotifyQuestUpdate::Opcode)
            {
                RequestQuestSyncAck ack;
                ack.Through = CastUnique<NotifyQuestUpdate>(std::move(message))->Through;
                Transmit(ack);
            }
            return;
        }

        if (message->GetOpcode() != NotifyQuestUpdate::Opcode)
            return;
        assert(Authed);
        const auto batch = CastUnique<NotifyQuestUpdate>(std::move(message));
        assert(Mode == "live" || Mode == "restarted");
        assert(batch->Updates.size() <= NotifyQuestUpdate::kMaxUpdates);
        if (Mode == "live" && FirstBatch.empty())
        {
            FirstBatch = batch->Updates;
            FirstThrough = batch->Through;
            return;
        }
        if (batch->Through == FirstThrough)
        {
            assert(batch->Updates == FirstBatch);
            Retried = true;
        }
        assert(batch->Through > LastThrough);
        LastThrough = batch->Through;
        for (const auto& event : batch->Updates)
        {
            assert(event.Id == GameId(QuestMod, 0x8AB));
            if (Received == 0)
                assert(event.Status == SharedQuestUpdate::Started && event.Stage == 0);
            else if (Received <= 20)
                assert(event.Status == SharedQuestUpdate::StageUpdate && event.Stage == Received);
            else if (Received == 21)
                assert(event.Status == SharedQuestUpdate::StageUpdate && event.Stage == 40);
            else if (Received == 22)
                assert(event.Status == SharedQuestUpdate::Stopped && event.Stage == 40);
            else if (Received == 23)
                assert(event.Status == SharedQuestUpdate::Completed && event.Stage == 40);
            else
                assert(false);
            ++Received;
        }
        RequestQuestSyncAck ack;
        ack.Through = LastThrough;
        Transmit(ack);
        if (Received == 24)
        {
            assert(LastThrough == 23);
            assert(Mode != "live" || Retried);
            assert(Mode != "restarted" || QuestMod == 2);
            Done = true;
        }
    }

    bool ConsumeDeathPacket(UniquePtr<ServerMessage>& aMessage)
    {
        if (aMessage->GetOpcode() == AssignCharacterResponse::Opcode)
        {
            const auto assignment = CastUnique<AssignCharacterResponse>(std::move(aMessage));
            assert(assignment->Owner);
            if (Mode == "death-patient")
            {
                DownedActorId = assignment->ServerId;
                ReportDowned(kReviveDownId);
            }
            else
            {
                std::puts("DEATH_READY");
                std::fflush(stdout);
            }
            return true;
        }

        if (aMessage->GetOpcode() == NotifyPlayerRespawn::Opcode)
        {
            assert(Mode == "death-patient");
            const auto grant = CastUnique<NotifyPlayerRespawn>(std::move(aMessage));
            if (grant->DownId == kReviveDownId)
            {
                assert(grant->Revived && grant->GoldLost == 0);
                if (!SawRevive)
                {
                    assert(std::chrono::steady_clock::now() - DownedAt >= 350ms);
                    SawRevive = true;
                    SendDeathRequest(PlayerRespawnRequest::Recovered, kReviveDownId);
                    DeathRequestAt = std::chrono::steady_clock::now() + 150ms;
                }
            }
            else
            {
                assert(SawRevive && grant->DownId == kRespawnDownId && !grant->Revived && grant->GoldLost == 10);
                if (!SawPenalty)
                {
                    assert(std::chrono::steady_clock::now() - DownedAt >= 1s);
                    SawPenalty = true;
                    SendDeathRequest(PlayerRespawnRequest::Recovered, kRespawnDownId);
                    SendDeathRequest(PlayerRespawnRequest::Respawn, kRespawnDownId);
                    OwnerReady = true;
                    ReadyAt = std::chrono::steady_clock::now() - 3s;
                }
            }
            return true;
        }

        if (aMessage->GetOpcode() == NotifyPlayerDowned::Opcode)
        {
            const auto notice = CastUnique<NotifyPlayerDowned>(std::move(aMessage));
            if (Mode == "death-patient")
                return true;
            if (notice->DownId == kReviveDownId && CurrentDownId == 0)
            {
                DownedActorId = notice->ActorId;
                CurrentDownId = notice->DownId;
                if (Mode == "death-helper")
                {
                    SendDeathRequest(PlayerRespawnRequest::Revive, kReviveDownId - 1, DownedActorId);
                    DeathRequestAt = std::chrono::steady_clock::now() + 400ms;
                }
                else
                    SendDeathRequest(PlayerRespawnRequest::Revive, kReviveDownId, DownedActorId);
            }
            else if (notice->ActorId == DownedActorId && !notice->DownId)
            {
                if (CurrentDownId == kReviveDownId)
                    SawRevive = true;
                else if (CurrentDownId == kRespawnDownId)
                {
                    assert(SawRevive);
                    OwnerReady = true;
                    ReadyAt = std::chrono::steady_clock::now() - 3s;
                }
            }
            else if (notice->ActorId == DownedActorId && notice->DownId == kRespawnDownId)
            {
                assert(SawRevive);
                CurrentDownId = notice->DownId;
            }
            return true;
        }

        if (aMessage->GetOpcode() == NotifyInventoryChanges::Opcode)
        {
            const auto change = CastUnique<NotifyInventoryChanges>(std::move(aMessage));
            if (change->ServerId == DownedActorId && change->Item.BaseId == GameId(VanillaMod, 0xF))
            {
                assert(Mode != "death-patient" && !SawPenalty);
                assert(CurrentDownId == kRespawnDownId && change->Item.Count == -10);
                SawPenalty = true;
            }
            return true;
        }
        if (aMessage->GetOpcode() == NotifyRespawn::Opcode)
        {
            const auto recovery = CastUnique<NotifyRespawn>(std::move(aMessage));
            if (recovery->ActorId == DownedActorId)
            {
                assert(Mode != "death-patient" && recovery->OwnershipEpoch);
                ++RemoteRecoveries;
            }
            return true;
        }
        return false;
    }

    void SendDeathRequest(uint8_t aAction, uint32_t aDownId, uint32_t aTarget = 0)
    {
        PlayerRespawnRequest request;
        request.Action = aAction;
        request.DownId = aDownId;
        request.TargetId = aTarget;
        Transmit(request);
    }

    void ReportDowned(uint32_t aDownId)
    {
        CurrentDownId = aDownId;
        DownedAt = std::chrono::steady_clock::now();
        if (aDownId == kRespawnDownId)
            SendDeathRequest(PlayerRespawnRequest::Downed, kReviveDownId);
        SendDeathRequest(PlayerRespawnRequest::Downed, aDownId);
        SendDeathRequest(PlayerRespawnRequest::Respawn, aDownId);
        if (aDownId == kReviveDownId)
            SendDeathRequest(PlayerRespawnRequest::Revive, aDownId, DownedActorId);
        else
            DeathRequestAt = DownedAt + 1250ms;
    }

    void AssignLock()
    {
        AssignObjectsRequest request;
        auto& object = request.Objects.emplace_back();
        object.Id = GameId(QuestMod, 0x8AD);
        object.CellId = GameId(VanillaMod, 0xA21);
        object.NoResetZone = true;
        object.IsContainer = true;
        auto& gold = object.CurrentInventory.Entries.emplace_back();
        gold.BaseId = GameId(VanillaMod, 0xF);
        gold.Count = 9;
        auto& questItem = object.CurrentInventory.Entries.emplace_back();
        questItem.BaseId = GameId(QuestMod, 0x8AC);
        questItem.IsQuestItem = true;
        questItem.Count = 1;
        object.CurrentLockData.IsLocked = true;
        object.CurrentLockData.LockLevel = 75;
        Transmit(request);
    }

    void AssignNpc(uint32_t aId = 0x899, bool aNonRespawning = true, bool aLeveled = false)
    {
        AssignCharacterRequest request{};
        request.Cookie = aId;
        request.ReferenceId = GameId(QuestMod, aId);
        request.IsNonRespawning = aNonRespawning;
        request.HasLeveledBase = aLeveled;
        request.CellId = GameId(VanillaMod, 0xA21);
        request.CurrentActorData.IsDead = Mode == "verify-npc";
        Transmit(request);
    }

    void SendWeather(uint32_t aForm)
    {
        RequestWeatherChange request{};
        request.Cell = GameId(VanillaMod, 0x510);
        request.Id = GameId(QuestMod, aForm);
        Transmit(request);
    }

    void SendEvent(uint8_t aKind, uint16_t aStage, uint32_t aBaseId = 0x8AB)
    {
        RequestQuestUpdate request{};
        request.Id = GameId(QuestMod, aBaseId);
        request.Stage = aStage;
        request.Status = aKind;
        request.ClientQuestType = 1;
        Transmit(request);
    }

    void Transmit(const ClientMessage& acMessage)
    {
        Buffer buffer(65536);
        Buffer::Writer writer(&buffer);
        writer.WriteBits(0, 8);
        acMessage.Serialize(writer);
        PacketView packet(reinterpret_cast<char*>(buffer.GetWriteData()), static_cast<uint32_t>(writer.Size()));
        Send(&packet);
    }

    std::string Mode;
    std::string Version;
    bool Authed{};
    bool Done{};
    bool Retried{};
    uint32_t QuestMod{};
    uint32_t VanillaMod{};
    uint32_t PlayerId{};
    bool SawNpc{};
    bool SawOwner{};
    bool OwnerReady{};
    bool SawCarriageTeleport{};
    std::chrono::steady_clock::time_point ReadyAt{};
    std::optional<std::chrono::steady_clock::time_point> DepartureAt;
    std::optional<std::chrono::steady_clock::time_point> DeathRequestAt;
    std::chrono::steady_clock::time_point DownedAt{};
    static constexpr uint32_t kReviveDownId = 41;
    static constexpr uint32_t kRespawnDownId = 42;
    uint32_t CurrentDownId{};
    uint32_t DownedActorId{};
    size_t RemoteRecoveries{};
    bool SawRevive{};
    bool SawPenalty{};
    size_t Received{};
    uint64_t FirstThrough{};
    uint64_t LastThrough{};
    std::vector<SharedQuestUpdate> FirstBatch;
    size_t AnimationCount{};
    static constexpr std::array<std::pair<const char*, uint64_t>, 3> kObjectAnimations{{
        {"ExampleStart", 1000}, {"ExampleLoop", 1040}, {"ExampleStop", 1100}
    }};
};

int main(int argc, char** argv)
{
    assert(argc == 4);
    CampaignClient client(argv[1], argv[2]);
    SteamNetworkingUtils()->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_Msg,
        [](ESteamNetworkingSocketsDebugOutputType, const char* acMessage)
        {
            std::fprintf(stderr, "%s\n", acMessage);
        });
    assert(client.ConnectByIp(argv[3]));
    const auto deadline = std::chrono::steady_clock::now() + (client.Mode == "write" || client.Mode == "missing" || client.Mode == "resurrect" ? 3s : 12s);
    while (!client.Done && std::chrono::steady_clock::now() < deadline)
    {
        client.Update();
        std::this_thread::sleep_for(5ms);
    }
    if (client.Mode == "write" || client.Mode == "missing" || client.Mode == "resurrect")
        assert(client.Authed && client.Received == 0 && (client.Mode == "missing" || client.SawNpc));
    else
        assert(client.Done);
    if (client.Mode.starts_with("death-"))
        assert(client.SawRevive && client.SawPenalty);
    if (client.Mode == "death-helper" || client.Mode == "death-distant")
        assert(client.RemoteRecoveries == 2);
    std::printf("%s passed (received %zu quest updates, retry %s)\n", argv[1], client.Received, client.Retried ? "yes" : "no");
    client.Close();
}
