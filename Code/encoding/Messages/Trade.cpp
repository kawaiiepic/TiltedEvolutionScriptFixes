#include <Messages/Trade.h>
#include <TiltedCore/Serialization.hpp>

void RequestTrade::SerializeRaw(TiltedPhoques::Buffer::Writer& w) const noexcept
{
    w.WriteBits(Action, 8);
    for (auto v : {Target, Session, Revision, Index, Quantity}) w.WriteBits(v, 32);
}
void RequestTrade::DeserializeRaw(TiltedPhoques::Buffer::Reader& r) noexcept
{
    ClientMessage::DeserializeRaw(r);
    uint64_t v{};
    r.ReadBits(v, 8); Action = static_cast<uint8_t>(v);
    for (auto* p : {&Target, &Session, &Revision, &Index, &Quantity})
    { r.ReadBits(v, 32); *p = static_cast<uint32_t>(v); }
}
void NotifyTrade::SerializeRaw(TiltedPhoques::Buffer::Writer& w) const noexcept
{
    w.WriteBits(Phase, 8);
    for (auto v : {Session, Revision, Partner}) w.WriteBits(v, 32);
    w.WriteBits(Incoming | (MineConfirmed << 1) | (OtherConfirmed << 2), 8);
    for (const auto* list : {&Mine, &Other})
    {
        const auto count = std::min<size_t>(Trade::MaxRows, list->size());
        w.WriteBits(count, 16);
        for (size_t i = 0; i < count; ++i)
        {
            const auto& row = (*list)[i];
            row.Item.Serialize(w);
            w.WriteBits(row.Available, 32); w.WriteBits(row.Offered, 32);
            w.WriteBits(row.Improved | (row.Enchanted << 1), 8);
        }
    }
}
void NotifyTrade::DeserializeRaw(TiltedPhoques::Buffer::Reader& r) noexcept
{
    ServerMessage::DeserializeRaw(r);
    uint64_t v{};
    r.ReadBits(v, 8); Phase = static_cast<uint8_t>(v);
    for (auto* p : {&Session, &Revision, &Partner})
    { r.ReadBits(v, 32); *p = static_cast<uint32_t>(v); }
    r.ReadBits(v, 8); Incoming = (v & 1) != 0; MineConfirmed = (v & 2) != 0; OtherConfirmed = (v & 4) != 0;
    for (auto* list : {&Mine, &Other})
    {
        list->clear(); r.ReadBits(v, 16);
        if (v > Trade::MaxRows) { Mine.clear(); Other.clear(); Phase = Trade::Error; return; }
        const size_t count = static_cast<size_t>(v);
        for (size_t i = 0; i < count; ++i)
        {
            Trade::Row row{};
            row.Item.Deserialize(r);
            r.ReadBits(v, 32); row.Available = static_cast<uint32_t>(v);
            r.ReadBits(v, 32); row.Offered = static_cast<uint32_t>(v);
            r.ReadBits(v, 8); row.Improved = (v & 1) != 0; row.Enchanted = (v & 2) != 0;
            list->push_back(row);
        }
    }
}
