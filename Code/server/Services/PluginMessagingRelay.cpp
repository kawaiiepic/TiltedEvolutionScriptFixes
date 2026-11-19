#include <Services/PluginMessagingRelay.h>

#include <GameServer.h>
#include <Messages/NotifyChatMessageBroadcast.h>
#include <PluginMessagingProtocol.h>
#include <Setting.h>
#include <World.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <string>

namespace
{
Console::Setting bPluginMessagingRelay{"Gameplay:bPluginMessagingRelay", "Relay STR Plugin Messaging API 0.10.1 traffic for connected clients", false};

constexpr size_t kMaxEnvelopeBytes = 4096;
constexpr uint32_t kMaxFragments = 64;
constexpr uint32_t kLoopbackFlag = 4;
constexpr uint32_t kSupportedFlags = 7;
constexpr std::string_view kIdentityChannel = "strpm.identity.v1";

enum Field : size_t { MessageId, Sequence, Channel, Target, Flags, Part, Parts, Payload, FieldCount };
constexpr std::array<std::string_view, FieldCount> kFieldNames{"msg", "seq", "channel", "target", "flags", "part", "parts", "payload"};

bool IsTokenCharacter(char aCharacter) noexcept
{
    return (aCharacter >= 'a' && aCharacter <= 'z') || (aCharacter >= 'A' && aCharacter <= 'Z') ||
           (aCharacter >= '0' && aCharacter <= '9') || aCharacter == '.' || aCharacter == '_' || aCharacter == '-';
}

bool IsHexCharacter(char aCharacter) noexcept
{
    return (aCharacter >= '0' && aCharacter <= '9') || (aCharacter >= 'a' && aCharacter <= 'f') ||
           (aCharacter >= 'A' && aCharacter <= 'F');
}

template <class T>
bool ParseUnsigned(std::string_view aText, T& aValue) noexcept
{
    if (aText.empty())
        return false;
    const auto parsed = std::from_chars(aText.data(), aText.data() + aText.size(), aValue);
    return parsed.ec == std::errc{} && parsed.ptr == aText.data() + aText.size();
}

bool ReadFields(std::string_view aBody, std::array<std::string_view, FieldCount>& aFields) noexcept
{
    std::array<bool, FieldCount> seen{};
    while (!aBody.empty())
    {
        const auto separator = aBody.find('|');
        const auto token = aBody.substr(0, separator);
        const auto equals = token.find('=');
        if (equals == token.npos)
            return false;

        const auto name = std::find(kFieldNames.begin(), kFieldNames.end(), token.substr(0, equals));
        if (name == kFieldNames.end())
            return false;
        const auto index = static_cast<size_t>(name - kFieldNames.begin());
        if (seen[index])
            return false;

        seen[index] = true;
        aFields[index] = token.substr(equals + 1);
        if (separator == aBody.npos)
            break;
        aBody.remove_prefix(separator + 1);
        if (aBody.empty())
            return false;
    }
    return std::all_of(seen.begin(), seen.end(), [](bool aSeen) { return aSeen; });
}
}

bool PluginMessagingRelay::IsEnabled() noexcept
{
    return bPluginMessagingRelay;
}

bool PluginMessagingRelay::ConsumeBudget(uint32_t aPlayerId, size_t aSize) noexcept
{
    constexpr float kBurst = 128.f;
    constexpr float kCreditsPerSecond = 64.f;
    const auto now = std::chrono::steady_clock::now();
    auto& budget = m_budgets.try_emplace(aPlayerId, Budget{now, kBurst}).first->second;
    const float elapsed = std::chrono::duration<float>(now - budget.LastUpdate).count();
    budget.Credits = std::min(kBurst, budget.Credits + elapsed * kCreditsPerSecond);
    budget.LastUpdate = now;
    const float cost = 1.f + static_cast<float>(aSize) / 1024.f;
    if (cost > budget.Credits)
        return false;
    budget.Credits -= cost;
    return true;
}

void PluginMessagingRelay::Forget(uint32_t aPlayerId) noexcept
{
    m_budgets.erase(aPlayerId);
}

void PluginMessagingRelay::Relay(World& aWorld, const Player& acSender, std::string_view aMessage) noexcept
{
    if (!aMessage.starts_with(PluginMessagingProtocol::Version2Prefix) || aMessage.size() > kMaxEnvelopeBytes ||
        !acSender.GetCharacter() || !acSender.GetCellComponent() || !ConsumeBudget(acSender.GetId(), aMessage.size()))
        return;

    std::array<std::string_view, FieldCount> fields{};
    uint64_t sequence{};
    uint32_t flags{}, part{}, parts{};
    if (!ReadFields(aMessage.substr(PluginMessagingProtocol::Version2Prefix.size()), fields) ||
        fields[MessageId].empty() || fields[MessageId].size() > 96 ||
        !std::all_of(fields[MessageId].begin(), fields[MessageId].end(), IsTokenCharacter) ||
        fields[Channel].empty() || fields[Channel].size() > 96 ||
        !std::all_of(fields[Channel].begin(), fields[Channel].end(), IsTokenCharacter) ||
        !std::all_of(fields[Payload].begin(), fields[Payload].end(), IsHexCharacter) ||
        !ParseUnsigned(fields[Sequence], sequence) ||
        !ParseUnsigned(fields[Flags], flags) || (flags & ~kSupportedFlags) ||
        !ParseUnsigned(fields[Part], part) || !ParseUnsigned(fields[Parts], parts) ||
        !part || part > parts || parts > kMaxFragments)
        return;

    if (fields[Target] == "server")
        return;

    const bool broadcast = fields[Target] == "all";
    uint64_t targetId{};
    if (!broadcast && (!fields[Target].starts_with("id:") ||
        !ParseUnsigned(fields[Target].substr(3), targetId) || !targetId ||
        targetId > std::numeric_limits<ConnectionId_t>::max()))
        return;

    std::string identityPayload;
    if (fields[Channel] == kIdentityChannel)
    {
        if (part != 1 || parts != 1 || !fields[Payload].empty())
            return;

        for (const char c : std::to_string(acSender.GetId()))
            identityPayload += fmt::format("{:02X}", static_cast<unsigned int>(static_cast<unsigned char>(c)));
        fields[Payload] = identityPayload;
    }

    std::string name(acSender.GetUsername().data(), acSender.GetUsername().size());
    std::replace_if(name.begin(), name.end(), [](char c) { return c == '|' || static_cast<unsigned char>(c) < 32; }, '_');

    // Rebuild the exact v2 wire envelope used by the upstream Lua relay.
    // All sender metadata comes from the authenticated connection.
    const auto relayed = fmt::format("STRPM|v2|msg={}|seq={}|channel={}|target={}|flags={}|part={}|parts={}|payload={}"
                                    "|sender={}|senderPlayerId={}|senderName={}|serverTick={}",
                                    fields[MessageId], sequence, fields[Channel], fields[Target], flags, part, parts, fields[Payload],
                                    acSender.GetConnectionId(), acSender.GetId(), name, GameServer::Get()->GetTick());
    NotifyChatMessageBroadcast notify{};
    notify.MessageType = kLocalChat;
    notify.PlayerName = "[Server]";
    notify.ChatMessage.assign(relayed.begin(), relayed.end());

    const auto canReceive = [&](const Player* apPlayer)
    {
        return apPlayer && apPlayer->GetCharacter() && apPlayer->GetCellComponent() &&
            (apPlayer != &acSender || (flags & kLoopbackFlag));
    };

    if (broadcast)
    {
        for (const auto* pPlayer : aWorld.GetPlayerManager())
            if (canReceive(pPlayer))
                pPlayer->Send(notify);
    }
    else if (const auto* pPlayer = aWorld.GetPlayerManager().GetByConnectionId(static_cast<ConnectionId_t>(targetId)); canReceive(pPlayer))
        pPlayer->Send(notify);
}
