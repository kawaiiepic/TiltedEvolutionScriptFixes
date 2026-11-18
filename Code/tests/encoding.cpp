#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <optional>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "StringCache.h"
#include "Messages/StringCacheUpdate.h"

#include <catch2/catch.hpp>

#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>
#include <Structs/Vector2_NetQuantize.h>

#include <TiltedCore/Math.hpp>
#include <TiltedCore/Platform.hpp>

using namespace TiltedPhoques;

TEST_CASE("Encoding factory", "[encoding.factory]")
{
    Buffer buff(1000);

    {
        AuthenticationRequest request;
        request.Token = "TesSt";

        Buffer::Writer writer(&buff);
        request.Serialize(writer);

        Buffer::Reader reader(&buff);

        const ClientMessageFactory factory;
        auto pMessage = factory.Extract(reader);

        REQUIRE(pMessage);
        REQUIRE(pMessage->GetOpcode() == request.GetOpcode());

        auto pRequest = CastUnique<AuthenticationRequest>(std::move(pMessage));
        REQUIRE(pRequest->Token == request.Token);
        REQUIRE(pRequest->Phase == AuthenticationRequest::Login);
    }

    {
        PartyAcceptInviteRequest request;
        request.InviterId = 123456;

        Buffer::Writer writer(&buff);
        request.Serialize(writer);

        Buffer::Reader reader(&buff);

        const ClientMessageFactory factory;
        auto pMessage = factory.Extract(reader);

        REQUIRE(pMessage);
        REQUIRE(pMessage->GetOpcode() == request.GetOpcode());

        auto pRequest = CastUnique<PartyAcceptInviteRequest>(std::move(pMessage));
        REQUIRE(pRequest->InviterId == request.InviterId);
    }
}

TEST_CASE("Character entry is a separate authenticated phase", "[encoding.auth]")
{
    AuthenticationRequest entry{};
    entry.Phase = AuthenticationRequest::EnterWorld;
    entry.Username = "Test character";
    entry.CellId = GameId(0x10003u, 0x456u);
    entry.UserMods.ModList.push_back({"Start.esl", 3, true});
    Buffer buffer(4096);
    Buffer::Writer writer(&buffer);
    entry.Serialize(writer);
    Buffer::Reader reader(&buffer);
    const ClientMessageFactory factory;
    auto message = factory.Extract(reader);
    REQUIRE(message);
    REQUIRE(*CastUnique<AuthenticationRequest>(std::move(message)) == entry);
    REQUIRE(entry.Token.empty());
}

TEST_CASE("Checkpoint packets have bounded exact wire size", "[encoding.saves]")
{
    RequestCharacterSave request{};
    request.Action = CharacterSavePayload::UploadChunk;
    request.Revision = 5;
    request.Offset = 17;
    request.Count = 3;
    request.Data[0] = 42;
    request.Data.back() = 255;
    Buffer buffer(CharacterSavePayload::WireSize + 16);
    Buffer::Writer writer(&buffer);
    request.Serialize(writer);
    REQUIRE(writer.Size() == CharacterSavePayload::WireSize);
    Buffer::Reader reader(&buffer);
    auto message = ClientMessageFactory{}.Extract(reader);
    REQUIRE(message);
    REQUIRE(message->GetOpcode() == RequestCharacterSave::Opcode);
    auto received = CastUnique<RequestCharacterSave>(std::move(message));
    REQUIRE(received->Action == request.Action);
    REQUIRE(received->Revision == request.Revision);
    REQUIRE(received->Offset == request.Offset);
    REQUIRE(received->Count == request.Count);
    REQUIRE(received->Data == request.Data);

    NotifyCharacterSave reply{};
    reply.Action = CharacterSavePayload::DownloadChunk;
    reply.Revision = 6;
    reply.Count = 3;
    reply.Data = request.Data;
    Buffer responseBuffer(CharacterSavePayload::WireSize + 16);
    Buffer::Writer responseWriter(&responseBuffer);
    reply.Serialize(responseWriter);
    REQUIRE(responseWriter.Size() == CharacterSavePayload::WireSize);
    Buffer::Reader responseReader(&responseBuffer);
    auto response = ServerMessageFactory{}.Extract(responseReader);
    REQUIRE(response);
    REQUIRE(response->GetOpcode() == NotifyCharacterSave::Opcode);
    auto decoded = CastUnique<NotifyCharacterSave>(std::move(response));
    REQUIRE(decoded->Revision == reply.Revision);
    REQUIRE(decoded->Data == reply.Data);
}

TEST_CASE("Static structures", "[encoding.static]")
{
    GIVEN("GameId")
    {
        GameId sendObjects, recvObjects;
        sendObjects.ModId = 1456987;
        sendObjects.BaseId = 0x789654;

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendObjects.Serialize(writer);

            Buffer::Reader reader(&buff);
            recvObjects.Deserialize(reader);

            REQUIRE(sendObjects == recvObjects);
        }
    }

    GIVEN("Vector3_NetQuantize")
    {
        Vector3_NetQuantize sendObjects, recvObjects;
        sendObjects.x = 142.56f;
        sendObjects.y = 45687.7f;
        sendObjects.z = -142.56f;

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendObjects.Serialize(writer);

            Buffer::Reader reader(&buff);
            recvObjects.Deserialize(reader);

            REQUIRE(sendObjects == recvObjects);
        }
    }

    GIVEN("Vector2_NetQuantize")
    {
        Vector2_NetQuantize sendObjects, recvObjects;
        sendObjects.x = 1000.89f;
        sendObjects.y = -485632.75f;

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendObjects.Serialize(writer);

            Buffer::Reader reader(&buff);
            recvObjects.Deserialize(reader);

            REQUIRE(sendObjects == recvObjects);
        }
    }

    GIVEN("Rotator2_NetQuantize")
    {
        Rotator2_NetQuantize sendObjects, recvObjects;
        sendObjects.x = 1.89f;
        sendObjects.y = TiltedPhoques::Pi * 2.0f;

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendObjects.Serialize(writer);

            Buffer::Reader reader(&buff);
            recvObjects.Deserialize(reader);

            REQUIRE(sendObjects == recvObjects);
        }
    }

    GIVEN("Rotator2_NetQuantize needing wrap")
    {
        // This test is a bit dangerous as floating errors can lead to sendObjects != recvObjects but the difference is minuscule so we don't care abut such cases
        Rotator2_NetQuantize sendObjects, recvObjects;
        sendObjects.x = -1.87f;
        sendObjects.y = static_cast<float>(TiltedPhoques::Pi) * 18.0f + 3.6f;

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendObjects.Serialize(writer);

            Buffer::Reader reader(&buff);
            recvObjects.Deserialize(reader);

            REQUIRE(sendObjects == recvObjects);
        }
    }
}

TEST_CASE("Differential structures", "[encoding.differential]")
{
    GIVEN("Full ActionEvent")
    {
        ActionEvent sendAction, recvAction;

        sendAction.ActionId = GameId(0, 42);
        sendAction.State1 = 6547;
        sendAction.Tick = 48;
        sendAction.ActorId = 12345678;
        sendAction.EventName = "test";
        sendAction.IdleId = GameId(7, 87964);
        sendAction.State2 = 8963;
        sendAction.TargetEventName = "toast";
        sendAction.TargetId = GameId(5, 963741);
        sendAction.TargetIdleId = GameId(9, 0xABC);
        sendAction.Type = 4;

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendAction.GenerateDifferential(recvAction, writer);

            Buffer::Reader reader(&buff);
            recvAction.ApplyDifferential(reader);

            REQUIRE(sendAction == recvAction);
        }

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendAction.EventName = "Plot twist !";
            sendAction.TargetIdleId = {};

            sendAction.GenerateDifferential(recvAction, writer);

            Buffer::Reader reader(&buff);
            recvAction.ApplyDifferential(reader);

            REQUIRE(sendAction == recvAction);
        }
    }

    GIVEN("A single cached event name")
    {
        ActionEvent sendAction, recvAction;

        TP_UNUSED(StringCache::Get().Add("test"))

        sendAction.ActionId = GameId(0, 42);
        sendAction.State1 = 6547;
        sendAction.Tick = 48;
        sendAction.ActorId = 12345678;
        sendAction.EventName = "test";
        sendAction.IdleId = GameId(7, 87964);
        sendAction.State2 = 8963;
        sendAction.TargetEventName = "toast";
        sendAction.TargetId = GameId(5, 963741);
        sendAction.Type = 4;

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendAction.GenerateDifferential(recvAction, writer);

            Buffer::Reader reader(&buff);
            recvAction.ApplyDifferential(reader);

            REQUIRE(sendAction == recvAction);
        }

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendAction.EventName = "Plot twist !";

            sendAction.GenerateDifferential(recvAction, writer);

            Buffer::Reader reader(&buff);
            recvAction.ApplyDifferential(reader);

            REQUIRE(sendAction == recvAction);
        }
    }

    GIVEN("Full Mods")
    {
        Mods sendMods, recvMods;

        Buffer buff(1000);
        Buffer::Writer writer(&buff);

        sendMods.ModList.push_back({"Hello", 42});
        sendMods.ModList.push_back({"Hi", 14});
        sendMods.ModList.push_back({"Test", 8});
        sendMods.ModList.push_back({"Toast", 49});

        sendMods.Serialize(writer);

        Buffer::Reader reader(&buff);
        recvMods.Deserialize(reader);

        REQUIRE(sendMods == recvMods);
    }

    GIVEN("AnimationVariables")
    {
        AnimationVariables vars, recvVars;
 
        vars.Booleans.resize(76);
        String testString("\xDE\xAD\xBE\xEF"
                          "\xDE\xAD\xBE\xEF\x76\xB");
        vars.String_to_VectorBool(testString, vars.Booleans);

        vars.Floats.push_back(1.f);
        vars.Floats.push_back(7.f);
        vars.Floats.push_back(12.f);
        vars.Floats.push_back(0.f);
        vars.Floats.push_back(145.f);
        vars.Floats.push_back(100.f);
        vars.Floats.push_back(-1.f);

        vars.Integers.push_back(0);
        vars.Integers.push_back(12000);
        vars.Integers.push_back(06);
        vars.Integers.push_back(7778);
        vars.Integers.push_back(41104539);

        Buffer buff(1000);
        {
            Buffer::Writer writer(&buff);

            vars.GenerateDiff(recvVars, writer);

            Buffer::Reader reader(&buff);
            recvVars.ApplyDiff(reader);

            REQUIRE(vars.Booleans == recvVars.Booleans);
            REQUIRE(vars.Floats == recvVars.Floats);
            REQUIRE(vars.Integers == recvVars.Integers);
        }

        vars.Booleans.resize(33);
        vars.Booleans[16] = false;
        vars.Booleans[17] = false;
        vars.Booleans[18] = false;
        vars.Booleans[19] = false;
        vars.Floats[3] = 42.f;
        vars.Integers[0] = 18;
        vars.Integers[3] = 0;

        {
            Buffer::Writer writer(&buff);

            vars.GenerateDiff(recvVars, writer);

            Buffer::Reader reader(&buff);
            recvVars.ApplyDiff(reader);

            REQUIRE(vars.Booleans == recvVars.Booleans);
            REQUIRE(vars.Floats == recvVars.Floats);
            REQUIRE(vars.Integers == recvVars.Integers);
        }
    }
}

TEST_CASE("Packets", "[encoding.packets]")
{
    SECTION("AuthenticationRequest")
    {
        Buffer buff(1000);

        AuthenticationRequest sendMessage, recvMessage;
        sendMessage.Token = "TesSt";
        sendMessage.UserMods.ModList.push_back({"Hello", 42});
        sendMessage.UserMods.ModList.push_back({"Hi", 14});
        sendMessage.UserMods.ModList.push_back({"Test", 8});
        sendMessage.UserMods.ModList.push_back({"Toast", 49});

        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);

        uint64_t trash;
        reader.ReadBits(trash, 8); // pop opcode

        recvMessage.DeserializeRaw(reader);

        REQUIRE(sendMessage == recvMessage);
    }

    SECTION("AuthenticationResponse")
    {
        Buffer buff(1000);

        AuthenticationResponse sendMessage, recvMessage;
        sendMessage.Type = AuthenticationResponse::ResponseType::kAccepted;
        sendMessage.UserMods.ModList.push_back({"Hello", 42});
        sendMessage.UserMods.ModList.push_back({"Hi", 14});
        sendMessage.UserMods.ModList.push_back({"Test", 8});
        sendMessage.UserMods.ModList.push_back({"Toast", 49});

        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);

        uint64_t trash;
        reader.ReadBits(trash, 8); // pop opcode

        recvMessage.DeserializeRaw(reader);

        REQUIRE(sendMessage == recvMessage);
    }

    SECTION("AssignCharacterRequest")
    {
        Buffer buff(1000);

        ActionEvent sendAction;
        sendAction.ActionId = GameId(0, 42);
        sendAction.State1 = 6547;
        sendAction.Tick = 48;
        sendAction.ActorId = 12345678;
        sendAction.EventName = "test";
        sendAction.IdleId = GameId(7, 87964);
        sendAction.State2 = 8963;
        sendAction.TargetEventName = "toast";
        sendAction.TargetId = GameId(5, 963741);
        sendAction.Type = 4;

        AssignCharacterRequest sendMessage, recvMessage;
        sendMessage.Cookie = 14523698;
        sendMessage.AppearanceBuffer = "toto";
        sendMessage.CellId.BaseId = 45;
        sendMessage.FormId.ModId = 48;
        sendMessage.ReferenceId.BaseId = 456799;
        sendMessage.ReferenceId.ModId = 4079;
        sendMessage.LatestAction = sendAction;
        sendMessage.Position.x = -452.4f;
        sendMessage.Position.y = 452.4f;
        sendMessage.Position.z = 125452.4f;
        sendMessage.Rotation.x = -1.87f;
        sendMessage.Rotation.y = 45.35f;
        sendMessage.IsNonRespawning = true;
        sendMessage.HasLeveledBase = true;

        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);

        uint64_t trash;
        reader.ReadBits(trash, 8); // pop opcode

        recvMessage.DeserializeRaw(reader);

        REQUIRE(sendMessage == recvMessage);
    }

    GIVEN("ClientReferencesMoveRequest")
    {
        ClientReferencesMoveRequest sendMessage, recvMessage;
        sendMessage.Tick = 1712345678901;
        auto& move = sendMessage.Updates[1];
        move.Position.x = 1234.5f;
        move.Direction = 0.75f;

        AnimationVariables vars;
        vars.Booleans.resize(76);
        String testString("\xDE\xAD\xBE\xEF\x76\xB");
        vars.String_to_VectorBool(testString, vars.Booleans);

        vars.Floats.push_back(1.f);
        vars.Floats.push_back(7.f);
        vars.Floats.push_back(12.f);
        vars.Floats.push_back(0.f);
        vars.Floats.push_back(145.f);
        vars.Floats.push_back(100.f);
        vars.Floats.push_back(-1.f);

        vars.Integers.push_back(0);
        vars.Integers.push_back(12000);
        vars.Integers.push_back(06);
        vars.Integers.push_back(7778);
        vars.Integers.push_back(41104539);

        move.Variables = vars;

        Buffer buff(1000);
        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);

        uint64_t trash;
        reader.ReadBits(trash, 8); // pop opcode

        recvMessage.DeserializeRaw(reader);

        REQUIRE(recvMessage.Tick == sendMessage.Tick);
        REQUIRE(recvMessage.Updates[1] == sendMessage.Updates[1]);
    }

    GIVEN("RequestActionEvents")
    {
        ActionEvent firstAction;
        firstAction.Tick = 1712345678901;
        firstAction.ActionId = GameId(0, 42);
        firstAction.TargetId = GameId(5, 963741);
        firstAction.State1 = 6547;
        firstAction.EventName = "jumpStandingStart";

        ActionEvent secondAction = firstAction;
        secondAction.Tick += 120;
        secondAction.ActionId = GameId(0, 43);
        secondAction.EventName = "attackStart";
        secondAction.TargetEventName = "toast";

        RequestActionEvents sendMessage, recvMessage;
        sendMessage.Actions[1] = {firstAction, secondAction};
        sendMessage.Actions[7] = {secondAction};

        Buffer buff(1000);
        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);

        uint64_t trash;
        reader.ReadBits(trash, 8); // pop opcode

        recvMessage.DeserializeRaw(reader);

        REQUIRE(sendMessage == recvMessage);
    }

    GIVEN("NotifyActionEvents")
    {
        ActionEvent action;
        action.Tick = 1712345678901;
        action.ActionId = GameId(0, 42);
        action.EventName = "blockStart";

        NotifyActionEvents sendMessage;
        sendMessage.Actions[3] = {action, action};

        Buffer buff(1000);
        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);

        const ServerMessageFactory factory;
        auto pMessage = factory.Extract(reader);

        REQUIRE(pMessage);
        REQUIRE(pMessage->GetOpcode() == sendMessage.GetOpcode());
        REQUIRE(*CastUnique<NotifyActionEvents>(std::move(pMessage)) == sendMessage);
    }
}

TEST_CASE("StringCache", "[encoding.string_cache]")
{
    SECTION("Messages")
    {
        StringCacheUpdate update;
        update.Values.push_back("Hello");
        update.Values.push_back("Bye");

        Buffer buff(1000);
        Buffer::Writer writer(&buff);
        update.Serialize(writer);

        Buffer::Reader reader(&buff);

        uint64_t trash;
        reader.ReadBits(trash, 8); // pop opcode

        StringCacheUpdate recvUpdate;
        recvUpdate.DeserializeRaw(reader);

        REQUIRE(update == recvUpdate);
    }
}

TEST_CASE("Shared dialogue protocol", "[encoding.dialogue]")
{
    SECTION("Owner publishes choices and spectator votes")
    {
        PlayerDialogueRequest request{};
        request.Action = PlayerDialogueRequest::PublishOptions;
        request.ActorId = 0xDEADBEEF;
        request.Token = 123;
        request.Revision = 456;
        request.Index = 1;
        request.Options = {"Tell me about the Greybeards.", "", "Goodbye."};
        request.Text = "Example";

        Buffer buff(4096);
        Buffer::Writer writer(&buff);
        request.Serialize(writer);
        Buffer::Reader reader(&buff);
        const ClientMessageFactory factory;
        auto message = factory.Extract(reader);
        REQUIRE(message);
        REQUIRE(message->GetOpcode() == request.GetOpcode());
        REQUIRE(*CastUnique<PlayerDialogueRequest>(std::move(message)) == request);
    }

    SECTION("Snapshot preserves option indices, counts and voter identities")
    {
        NotifyPlayerDialogue panel{};
        panel.Kind = NotifyPlayerDialogue::Panel;
        panel.CanPass = true;
        panel.Private = false;
        panel.ActorId = 0xDEADBEEF;
        panel.Revision = 7;
        panel.Selected = 2;
        panel.Name = "Owner";
        panel.Options = {"Yes", "", "No"};
        panel.Votes = {1, 0, 3};
        panel.VoterChoices = {{11, 0}, {12, 2}, {13, 2}, {14, 2}};

        Buffer buff(4096);
        Buffer::Writer writer(&buff);
        panel.Serialize(writer);
        Buffer::Reader reader(&buff);
        const ServerMessageFactory factory;
        auto message = factory.Extract(reader);
        REQUIRE(message);
        REQUIRE(message->GetOpcode() == panel.GetOpcode());
        REQUIRE(*CastUnique<NotifyPlayerDialogue>(std::move(message)) == panel);
    }
}

TEST_CASE("Faction snapshots keep their ownership epoch", "[encoding.factions]")
{
    Faction faction;
    faction.Id = GameId(3, 0x812);
    faction.Rank = 4;

    RequestFactionsChanges request;
    request.Changes[7].OwnershipEpoch = 42;
    request.Changes[7].Content.ExtraFactions.push_back(faction);
    Buffer requestBuffer(4096);
    Buffer::Writer requestWriter(&requestBuffer);
    request.Serialize(requestWriter);
    Buffer::Reader requestReader(&requestBuffer);
    auto receivedRequest = ClientMessageFactory{}.Extract(requestReader);
    REQUIRE(receivedRequest);
    REQUIRE(*CastUnique<RequestFactionsChanges>(std::move(receivedRequest)) == request);

    NotifyOwnershipTransfer transfer;
    transfer.ServerId = 7;
    transfer.OwnershipEpoch = 43;
    transfer.FactionsContent = request.Changes[7].Content;
    transfer.FactionsContent.NpcFactions.resize(512, faction);
    Buffer transferBuffer(8192);
    Buffer::Writer transferWriter(&transferBuffer);
    transfer.Serialize(transferWriter);
    Buffer::Reader transferReader(&transferBuffer);
    auto receivedTransfer = ServerMessageFactory{}.Extract(transferReader);
    REQUIRE(receivedTransfer);
    transfer.FactionsContent.NpcFactions.pop_back();
    REQUIRE(*CastUnique<NotifyOwnershipTransfer>(std::move(receivedTransfer)) == transfer);
}

TEST_CASE("Shared scripted scene protocol", "[encoding.scene]")
{
    SECTION("Native observer snapshot")
    {
        RequestSceneState request{};
        request.State.Id = GameId(2, 0xFABCD);
        request.State.Quest = GameId(0, 0x12345);
        request.State.Phase = UINT32_MAX;
        request.State.Active = true;
        request.State.CompletedActions = {3, 9, 42};
        request.State.ReferenceAliases = {{7, GameId(2, 0x33445)}, {12, GameId(0, 0x54321)}};

        Buffer buff(4096);
        Buffer::Writer writer(&buff);
        request.Serialize(writer);
        Buffer::Reader reader(&buff);
        const ClientMessageFactory factory;
        auto message = factory.Extract(reader);
        REQUIRE(message);
        REQUIRE(message->GetOpcode() == request.GetOpcode());
        REQUIRE(*CastUnique<RequestSceneState>(std::move(message)) == request);
    }

    SECTION("Server ownership and generation")
    {
        NotifySceneState snapshot{};
        snapshot.State.Id = GameId(1, 0x12345);
        snapshot.State.Quest = GameId(1, 0x34567);
        snapshot.State.Phase = 6;
        snapshot.State.Active = true;
        snapshot.State.CompletedActions = {12, 13};
        snapshot.State.ReferenceAliases = {{3, GameId(4, 0x777)}};
        snapshot.Owner = 4;
        snapshot.Generation = 9;

        Buffer buff(4096);
        Buffer::Writer writer(&buff);
        snapshot.Serialize(writer);
        Buffer::Reader reader(&buff);
        const ServerMessageFactory factory;
        auto message = factory.Extract(reader);
        REQUIRE(message);
        REQUIRE(message->GetOpcode() == snapshot.GetOpcode());
        REQUIRE(*CastUnique<NotifySceneState>(std::move(message)) == snapshot);
    }
}

TEST_CASE("Campaign replay batch and acknowledgement round trip", "[encoding.campaign]")
{
    constexpr uint64_t cThrough = (1ull << 40) + 12;

    NotifyQuestUpdate batch;
    batch.Through = cThrough;
    batch.Updates = {
        {GameId(1, 0xABCDEF), 10, SharedQuestUpdate::Started},
        {GameId(0, 0x123456), 40, SharedQuestUpdate::StageUpdate},
        {GameId(0, 0x123456), 40, SharedQuestUpdate::Stopped},
    };
    {
        Buffer buffer(1024);
        Buffer::Writer writer(&buffer);
        batch.Serialize(writer);
        Buffer::Reader reader(&buffer);
        auto message = ServerMessageFactory{}.Extract(reader);
        REQUIRE(message);
        REQUIRE(message->GetOpcode() == NotifyQuestUpdate::Opcode);
        const auto decoded = CastUnique<NotifyQuestUpdate>(std::move(message));
        REQUIRE(decoded->Through == cThrough);
        REQUIRE(decoded->Updates == batch.Updates);
    }

    RequestQuestSyncAck ack;
    ack.Through = cThrough;
    {
        Buffer buffer(1024);
        Buffer::Writer writer(&buffer);
        ack.Serialize(writer);
        Buffer::Reader reader(&buffer);
        auto message = ClientMessageFactory{}.Extract(reader);
        REQUIRE(message);
        REQUIRE(message->GetOpcode() == RequestQuestSyncAck::Opcode);
        REQUIRE(CastUnique<RequestQuestSyncAck>(std::move(message))->Through == cThrough);
    }
}

TEST_CASE("First object assignment can restore an unlocked campaign lock", "[encoding.campaign]")
{
    AssignObjectsResponse response;
    auto& object = response.Objects.emplace_back();
    object.Id = GameId(1, 0x18B);
    object.IsSenderFirst = true;
    object.HasCampaignLock = true;
    object.NoResetZone = true;
    object.IsContainer = true;
    object.HasCampaignStock = true;
    object.CellId = GameId(0, 0x804);

    Buffer buffer(1024);
    Buffer::Writer writer(&buffer);
    response.Serialize(writer);
    Buffer::Reader reader(&buffer);
    auto message = ServerMessageFactory{}.Extract(reader);
    REQUIRE(message);
    REQUIRE(message->GetOpcode() == AssignObjectsResponse::Opcode);
    REQUIRE(CastUnique<AssignObjectsResponse>(std::move(message))->Objects == response.Objects);
}

TEST_CASE("Cell-scoped messages preserve plugin identity", "[encoding.campaign]")
{
    Buffer buffer(1024);
    {
        NotifyWeatherChange weather{};
        weather.Cell = GameId(1, 0x421);
        weather.Id = GameId(2, 0xABC);
        weather.Owner = 34;
        Buffer::Writer writer(&buffer);
        weather.Serialize(writer);
        Buffer::Reader reader(&buffer);
        auto message = ServerMessageFactory{}.Extract(reader);
        REQUIRE(message);
        REQUIRE(message->GetOpcode() == NotifyWeatherChange::Opcode);
        REQUIRE(*CastUnique<NotifyWeatherChange>(std::move(message)) == weather);
    }
    {
        ScriptAnimationRequest animation{};
        animation.Cell = GameId(1, 0x421);
        animation.Id = GameId(5, 0x8AB);
        animation.Tick = 123456;
        animation.EventName = "ExampleGraphEvent";
        Buffer::Writer writer(&buffer);
        animation.Serialize(writer);
        Buffer::Reader reader(&buffer);
        auto message = ClientMessageFactory{}.Extract(reader);
        REQUIRE(message);
        REQUIRE(message->GetOpcode() == ScriptAnimationRequest::Opcode);
        REQUIRE(*CastUnique<ScriptAnimationRequest>(std::move(message)) == animation);
    }
}

TEST_CASE("Carriage travel retains the boarding token", "[encoding.travel]")
{
    RequestCarriageTravel arrival;
    arrival.Seat = GameId(2, 0x812);
    arrival.Cell = GameId(3, 0x928);
    arrival.Position = glm::vec3(64.f, -128.f, 256.f);
    arrival.Token = 17;
    arrival.Arrived = true;

    Buffer buffer(1024);
    Buffer::Writer writer(&buffer);
    arrival.Serialize(writer);
    Buffer::Reader reader(&buffer);
    auto message = ClientMessageFactory{}.Extract(reader);
    REQUIRE(message);
    REQUIRE(message->GetOpcode() == RequestCarriageTravel::Opcode);
    const auto parsed = CastUnique<RequestCarriageTravel>(std::move(message));
    REQUIRE(parsed->Seat == arrival.Seat);
    REQUIRE(parsed->Cell == arrival.Cell);
    REQUIRE(parsed->Position == arrival.Position);
    REQUIRE(parsed->Token == arrival.Token);
    REQUIRE(parsed->Arrived);

    NotifyTeleport teleport;
    teleport.CellId = arrival.Cell;
    teleport.WorldSpaceId = GameId(0, 0x3C);
    teleport.Position = arrival.Position;
    teleport.CarriageToken = 4;
    Buffer::Writer teleportWriter(&buffer);
    teleport.Serialize(teleportWriter);
    Buffer::Reader teleportReader(&buffer);
    auto received = ServerMessageFactory{}.Extract(teleportReader);
    REQUIRE(received);
    REQUIRE(received->GetOpcode() == NotifyTeleport::Opcode);
    REQUIRE(*CastUnique<NotifyTeleport>(std::move(received)) == teleport);
}

TEST_CASE("Player revive keeps the target and downed episode", "[encoding.respawn]")
{
    PlayerRespawnRequest request;
    request.Action = PlayerRespawnRequest::Revive;
    request.DownId = 23;
    request.TargetId = 0x10002;

    Buffer buffer(1024);
    Buffer::Writer writer(&buffer);
    request.Serialize(writer);
    Buffer::Reader reader(&buffer);
    auto message = ClientMessageFactory{}.Extract(reader);
    REQUIRE(message);
    REQUIRE(message->GetOpcode() == PlayerRespawnRequest::Opcode);
    REQUIRE(*CastUnique<PlayerRespawnRequest>(std::move(message)) == request);

    NotifyPlayerDowned downed;
    downed.ActorId = request.TargetId;
    downed.DownId = request.DownId;
    Buffer::Writer downedWriter(&buffer);
    downed.Serialize(downedWriter);
    Buffer::Reader downedReader(&buffer);
    auto notice = ServerMessageFactory{}.Extract(downedReader);
    REQUIRE(notice);
    REQUIRE(notice->GetOpcode() == NotifyPlayerDowned::Opcode);
    auto parsed = CastUnique<NotifyPlayerDowned>(std::move(notice));
    REQUIRE(parsed->ActorId == request.TargetId);
    REQUIRE(parsed->DownId == request.DownId);

    NotifyPlayerRespawn resolved;
    resolved.DownId = request.DownId;
    resolved.Revived = true;
    Buffer::Writer resolvedWriter(&buffer);
    resolved.Serialize(resolvedWriter);
    Buffer::Reader resolvedReader(&buffer);
    auto grant = ServerMessageFactory{}.Extract(resolvedReader);
    REQUIRE(grant);
    REQUIRE(grant->GetOpcode() == NotifyPlayerRespawn::Opcode);
    REQUIRE(*CastUnique<NotifyPlayerRespawn>(std::move(grant)) == resolved);
}

TEST_CASE("Dropping missing stock does not create a negative inventory row", "[encoding.campaign]")
{
    Inventory inventory;
    Inventory::Entry item{};
    item.BaseId = GameId(0, 0xF);
    item.Count = -5;
    inventory.AddOrRemoveEntry(item);
    REQUIRE(inventory.Entries.empty());

    item.Count = INT32_MAX;
    inventory.AddOrRemoveEntry(item);
    inventory.AddOrRemoveEntry(item);
    REQUIRE(inventory.Entries.size() == 1);
    REQUIRE(inventory.Entries.front().Count == INT32_MAX);
}

TEST_CASE("Movement packets retain the ownership grant", "[encoding.campaign]")
{
    ClientReferencesMoveRequest movement{};
    movement.Tick = 420;
    movement.Updates[77].OwnershipEpoch = 32;
    movement.Updates[77].CellId = GameId(0, 0x501);

    Buffer buffer(1024);
    Buffer::Writer writer(&buffer);
    movement.Serialize(writer);
    Buffer::Reader reader(&buffer);
    auto message = ClientMessageFactory{}.Extract(reader);
    REQUIRE(message);
    REQUIRE(message->GetOpcode() == ClientReferencesMoveRequest::Opcode);
    REQUIRE(*CastUnique<ClientReferencesMoveRequest>(std::move(message)) == movement);
}
