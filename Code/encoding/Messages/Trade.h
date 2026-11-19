#pragma once
#include "Message.h"
#include <Structs/GameId.h>

namespace Trade
{
    enum Action : uint8_t { Invite, Accept, Offer, Confirm, Cancel };
    enum Phase : uint8_t { Pending, Open, Completed, Closed, Error };
    inline constexpr uint32_t MaxRows = 256;
    struct Row
    {
        GameId Item{};
        uint32_t Available{}, Offered{};
        bool Improved{}, Enchanted{};
    };
}
struct RequestTrade final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestTrade;
    RequestTrade() : ClientMessage(Opcode) {}
    uint8_t Action{};
    uint32_t Target{}, Session{}, Revision{}, Index{}, Quantity{};
    void SerializeRaw(TiltedPhoques::Buffer::Writer&) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader&) noexcept override;
    static bool ValidPacket(const void*, uint32_t size) noexcept { return size == 22; }
};
struct NotifyTrade final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyTrade;
    NotifyTrade() : ServerMessage(Opcode) {}
    uint8_t Phase{};
    uint32_t Session{}, Revision{}, Partner{};
    bool Incoming{}, MineConfirmed{}, OtherConfirmed{};
    TiltedPhoques::Vector<Trade::Row> Mine, Other;
    void SerializeRaw(TiltedPhoques::Buffer::Writer&) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader&) noexcept override;
};
