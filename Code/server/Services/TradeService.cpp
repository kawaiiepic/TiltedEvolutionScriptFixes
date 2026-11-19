#include <Services/TradeService.h>
#include <World.h>
#include <Components.h>
#include <Game/Player.h>
#include <GameServer.h>
#include <Events/PlayerLeaveEvent.h>
#include <Events/UpdateEvent.h>
#include <Messages/NotifyInventoryChanges.h>
#include <algorithm>
#include <cmath>

namespace
{
bool Tradable(const Inventory::Entry& e) noexcept
{
    return e.BaseId && e.Count > 0 && !e.IsQuestItem && !e.IsWorn();
}
bool Same(const Inventory::Entry& a, const Inventory::Entry& b) noexcept
{
    if (!a.CanBeMerged(b) || a.EnchantData.IsWeapon != b.EnchantData.IsWeapon ||
        a.EnchantData.Effects.size() != b.EnchantData.Effects.size()) return false;
    for (size_t i = 0; i < a.EnchantData.Effects.size(); ++i)
    {
        const auto& x = a.EnchantData.Effects[i];
        const auto& y = b.EnchantData.Effects[i];
        if (x.Magnitude != y.Magnitude || x.Area != y.Area || x.Duration != y.Duration ||
            x.RawCost != y.RawCost || x.EffectId != y.EffectId) return false;
    }
    return true;
}
}
TradeService::TradeService(World& world, entt::dispatcher& dispatcher) noexcept : m_world(world)
{
    m_request = dispatcher.sink<PacketEvent<RequestTrade>>().connect<&TradeService::OnRequest>(this);
    m_leave = dispatcher.sink<PlayerLeaveEvent>().connect<&TradeService::OnLeave>(this);
    m_update = dispatcher.sink<UpdateEvent>().connect<&TradeService::OnUpdate>(this);
}
uint32_t TradeService::Find(uint32_t player) const noexcept
{
    for (const auto& [id, s] : m_sessions)
        if (s.Sides[0].PlayerId == player || s.Sides[1].PlayerId == player) return id;
    return 0;
}
bool TradeService::Nearby(const Player& a, const Player& b) const noexcept
{
    if (!a.GetCharacter() || !b.GetCharacter() || !a.GetCellComponent() ||
        a.GetCellComponent().Cell != b.GetCellComponent().Cell) return false;
    const auto* x = m_world.try_get<MovementComponent>(*a.GetCharacter());
    const auto* y = m_world.try_get<MovementComponent>(*b.GetCharacter());
    if (!x || !y) return false;
    const auto d = x->Position - y->Position;
    return glm::dot(d, d) <= 400.f * 400.f;
}
bool TradeService::Capture(Side& side, Player& p) noexcept
{
    const auto character = p.GetCharacter();
    if (!character) return false;
    const auto* inventory = m_world.try_get<InventoryComponent>(*character);
    if (!inventory) return false;
    side.Catalog.clear();
    for (const auto& e : inventory->Content.Entries)
    {
        if (!Tradable(e)) continue;
        if (side.Catalog.size() >= Trade::MaxRows) break;
        side.Catalog.push_back(e);
    }
    side.Offered.assign(side.Catalog.size(), 0);
    return true;
}
void TradeService::Publish(const Session& s, uint8_t phase) noexcept
{
    for (size_t i = 0; i < 2; ++i)
    {
        const auto& mine = s.Sides[i];
        const auto& other = s.Sides[1-i];
        auto* p = m_world.GetPlayerManager().GetById(mine.PlayerId);
        if (!p) continue;
        NotifyTrade msg{};
        msg.Phase = phase; msg.Session = s.Id; msg.Revision = s.Revision;
        msg.Partner = other.PlayerId; msg.Incoming = i == 1;
        msg.MineConfirmed = mine.Confirmed; msg.OtherConfirmed = other.Confirmed;
        if (phase == Trade::Open)
        {
            const auto append = [](const Side& side, auto& rows, bool offersOnly)
            {
                for (size_t j = 0; j < side.Catalog.size(); ++j)
                {
                    const auto& e = side.Catalog[j];
                    if (offersOnly && !side.Offered[j]) continue;
                    rows.push_back(Trade::Row{e.BaseId, static_cast<uint32_t>(e.Count), side.Offered[j],
                        e.ExtraHealth > 1.f, bool(e.ExtraEnchantId) || !e.EnchantData.Effects.empty()});
                }
            };
            append(mine, msg.Mine, false); append(other, msg.Other, true);
        }
        p->Send(msg);
    }
}
void TradeService::Close(uint32_t id, uint8_t phase) noexcept
{
    const auto it = m_sessions.find(id);
    if (it == m_sessions.end()) return;
    Publish(it->second, phase);
    m_sessions.erase(it);
}
void TradeService::Error(Player& p) noexcept
{
    NotifyTrade msg{}; msg.Phase = Trade::Error; p.Send(msg);
}
bool TradeService::Commit(Session& s, Player& a, Player& b) noexcept
{
    // This is a trusted-client feature, not an anti-cheat transaction ledger.
    // Validate all outgoing stacks before mutating either live server inventory.
    if (!Nearby(a,b)) return false;
    const std::array<Player*,2> players{&a,&b};
    std::array<InventoryComponent*,2> inventories{};
    std::array<Inventory,2> after{};
    std::array<TiltedPhoques::Vector<Inventory::Entry>,2> transfers{};
    for (size_t i = 0; i < 2; ++i)
    {
        const auto character = players[i]->GetCharacter();
        if (!character) return false;
        inventories[i] = m_world.try_get<InventoryComponent>(*character);
        const auto* owner = m_world.try_get<OwnerComponent>(*character);
        if (!inventories[i] || !owner || owner->GetOwner() != players[i] || !owner->OwnershipEpoch) return false;
        after[i] = inventories[i]->Content;
        const auto& side = s.Sides[i];
        for (size_t j = 0; j < side.Catalog.size(); ++j)
        {
            const auto count = side.Offered[j];
            if (!count) continue;
            const auto& original = side.Catalog[j];
            const auto it = std::find_if(after[i].Entries.begin(), after[i].Entries.end(),
                [&](const auto& e) { return Tradable(e) && Same(e, original) && static_cast<uint32_t>(e.Count) >= count; });
            if (it == after[i].Entries.end()) return false;
            auto item = *it;
            item.Count = static_cast<int32_t>(count);
            transfers[i].push_back(item);
            it->Count -= item.Count;
            if (!it->Count) after[i].Entries.erase(it);
        }
    }
    if (transfers[0].empty() && transfers[1].empty()) return false;
    for (size_t i = 0; i < 2; ++i)
        for (const auto& e : transfers[1-i])
        {
            auto& entries = after[i].Entries;
            auto it = std::find_if(entries.begin(), entries.end(), [&](const auto& existing){return Same(existing,e);});
            if (it == entries.end()) entries.push_back(e);
            else
            {
                if (static_cast<int64_t>(it->Count) + e.Count > INT32_MAX) return false;
                it->Count += e.Count;
            }
        }
    for (size_t i = 0; i < 2; ++i) inventories[i]->Content = std::move(after[i]);
    for (size_t i = 0; i < 2; ++i)
    {
        const auto entity = *players[i]->GetCharacter();
        const auto& owner = m_world.get<OwnerComponent>(entity);
        const auto send = [&](Inventory::Entry item)
        {
            NotifyInventoryChanges msg{};
            msg.ServerId = World::ToInteger(entity);
            msg.OwnershipEpoch = owner.OwnershipEpoch; msg.Item = std::move(item);
            // Direct owner update uses the existing ScopedInventoryOverride path.
            players[i]->Send(msg);
            GameServer::Get()->SendToPlayersInRange(msg, entity, players[i]);
        };
        for (auto item : transfers[i]) { item.Count = -item.Count; send(std::move(item)); }
        for (const auto& item : transfers[1-i]) send(item);
    }
    return true;
}
void TradeService::OnRequest(const PacketEvent<RequestTrade>& event) noexcept
{
    auto& p = *event.pPlayer;
    const auto& r = event.Packet;
    const uint32_t existing = Find(p.GetId());
    if (r.Action == Trade::Invite)
    {
        auto* target = m_world.GetPlayerManager().GetById(r.Target);
        if (existing || !target || target == &p || Find(r.Target) || !Nearby(p,*target))
        { if (!existing) Error(p); return; }
        Session s{};
        if (++m_nextId == 0) ++m_nextId;
        s.Id = m_nextId;
        s.Sides[0].PlayerId = p.GetId(); s.Sides[1].PlayerId = r.Target;
        s.Expires = Clock::now() + std::chrono::seconds(30);
        auto it = m_sessions.emplace(s.Id,std::move(s)).first;
        Publish(it->second, Trade::Pending); return;
    }
    if (!existing || existing != r.Session) return;
    auto& s = m_sessions.at(existing);
    const size_t i = s.Sides[0].PlayerId == p.GetId() ? 0 : 1;
    auto* other = m_world.GetPlayerManager().GetById(s.Sides[1-i].PlayerId);
    if (r.Action == Trade::Cancel || !other || !Nearby(p,*other)) { Close(existing,Trade::Closed); return; }
    if (r.Revision != s.Revision) { Publish(s,s.Accepted ? Trade::Open : Trade::Pending); return; }
    if (!s.Accepted)
    {
        if (r.Action != Trade::Accept || i != 1) return;
        if (!Capture(s.Sides[0],*other) || !Capture(s.Sides[1],p)) { Close(existing,Trade::Error); return; }
        s.Accepted = true; ++s.Revision;
    }
    else if (r.Action == Trade::Offer)
    {
        auto& side = s.Sides[i];
        if (r.Index >= side.Catalog.size() || r.Quantity > static_cast<uint32_t>(side.Catalog[r.Index].Count)) return;
        if (side.Offered[r.Index] != r.Quantity)
        {
            side.Offered[r.Index] = r.Quantity;
            s.Sides[0].Confirmed = s.Sides[1].Confirmed = false;
            ++s.Revision;
        }
    }
    else if (r.Action == Trade::Confirm)
    {
        s.Sides[i].Confirmed = true;
        if (s.Sides[0].Confirmed && s.Sides[1].Confirmed)
        {
            auto* a = m_world.GetPlayerManager().GetById(s.Sides[0].PlayerId);
            auto* b = m_world.GetPlayerManager().GetById(s.Sides[1].PlayerId);
            const bool ok = a && b && Commit(s,*a,*b);
            Close(existing,ok ? Trade::Completed : Trade::Error); return;
        }
    }
    else return;
    s.Expires = Clock::now()+std::chrono::minutes(2);
    Publish(s,Trade::Open);
}
void TradeService::OnLeave(const PlayerLeaveEvent& event) noexcept
{
    if (const auto id = Find(event.pPlayer->GetId())) Close(id,Trade::Closed);
}
void TradeService::OnUpdate(const UpdateEvent&) noexcept
{
    const auto now = Clock::now();
    if (now < m_nextTick) return;
    m_nextTick = now + std::chrono::seconds(1);
    for (auto it = m_sessions.begin(); it != m_sessions.end();)
    {
        const auto id = it->first;
        auto& s = it->second; ++it;
        auto* a = m_world.GetPlayerManager().GetById(s.Sides[0].PlayerId);
        auto* b = m_world.GetPlayerManager().GetById(s.Sides[1].PlayerId);
        if (now >= s.Expires || !a || !b || !Nearby(*a,*b)) Close(id,Trade::Closed);
    }
}
