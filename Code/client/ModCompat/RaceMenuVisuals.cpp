#include <TiltedOnlinePCH.h>

#include <ModCompat/RaceMenuVisuals.h>
#include <Actor.h>
#include <Forms/TESNPC.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <tuple>

namespace
{
using namespace RaceMenuVisuals;

// MSVC x64 slots from RaceMenu's public IPluginInterface.h. Each table is
// checked against its published interface version before use.
template <size_t Slot, class Result, class... Args>
Result Invoke(void* apInterface, Args... aArgs)
{
    using Function = Result (*)(void*, Args...);
    return reinterpret_cast<Function>((*static_cast<void***>(apInterface))[Slot])(apInterface, aArgs...);
}

void* s_overlay{};
void* s_override{};
void* s_transform{};

struct GetValue
{
    virtual void Int(int32_t aValue) { Integer = aValue; }
    virtual void Float(float aValue) { Number = aValue; }
    virtual void String(const char* apValue) { if (apValue) Text = apValue; }
    virtual void Bool(bool) {}
    virtual void TextureSet(const BGSTextureSet*) {}
    int32_t Integer{0xFFFFFF};
    float Number{1.f};
    std::string Text;
};

struct SetValue
{
    enum Type : int32_t { None, Integer, Number, Text };
    virtual Type GetType() { return Kind; }
    virtual int32_t Int() { return Value.Integer; }
    virtual float Float() { return Value.Number; }
    virtual const char* String() { return Value.Text; }
    virtual bool Bool() { return false; }
    virtual BGSTextureSet* TextureSet() { return nullptr; }

    Type Kind{};
    union { int32_t Integer; float Number; const char* Text; } Value{};
};

struct Triplet { float Values[3]; };

bool ValidToken(std::string_view aText)
{
    return !aText.empty() && aText.size() <= 100 &&
        std::ranges::none_of(aText, [](unsigned char c) { return c < 32 || c == 127; });
}

bool ValidTexture(std::string_view aText)
{
    if (aText.size() > 220 || aText.find("..") != aText.npos)
        return false;
    std::string normalized(aText);
    for (auto& c : normalized)
    {
        if (c == '/')
            c = '\\';
        else if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + ('a' - 'A'));
    }
    return !normalized.starts_with('\\') && normalized.ends_with(".dds") &&
        !normalized.ends_with("actors\\character\\overlays\\default.dds") &&
        std::ranges::all_of(normalized, [](char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                c == ' ' || c == '.' || c == '_' || c == '-' || c == '\\';
        });
}

bool ValidTransform(const Transform& acValue)
{
    if (!ValidToken(acValue.Node) || !ValidToken(acValue.Key) || acValue.Type > Transform::ScaleMode)
        return false;
    for (const float value : acValue.Values)
        if (!std::isfinite(value) || std::abs(value) > 1000.f)
            return false;
    if (acValue.Type == Transform::Scale)
        return acValue.Values[0] >= 0.01f && acValue.Values[0] <= 10.f;
    if (acValue.Type == Transform::ScaleMode)
        return acValue.Values[0] >= 0.f && acValue.Values[0] <= 4.f &&
            std::floor(acValue.Values[0]) == acValue.Values[0];
    return true;
}

struct NodeVisitor
{
    virtual bool VisitPosition(const char* pNode, const char* pKey, Triplet& aValue) { return Add(pNode, pKey, Transform::Position, aValue); }
    virtual bool VisitRotation(const char* pNode, const char* pKey, Triplet& aValue) { return Add(pNode, pKey, Transform::Rotation, aValue); }
    virtual bool VisitScale(const char* pNode, const char* pKey, float aValue) { return Add(pNode, pKey, Transform::Scale, {{aValue, 0, 0}}); }
    virtual bool VisitScaleMode(const char* pNode, const char* pKey, uint32_t aValue)
    {
        return Add(pNode, pKey, Transform::ScaleMode, {{static_cast<float>(aValue), 0, 0}});
    }

    bool Add(const char* apNode, const char* apKey, uint8_t aType, const Triplet& acValue)
    {
        if (apNode && apKey && !std::string_view(apKey).starts_with("SkyrimTogether/") && Values.size() < 96)
        {
            Transform record{apNode, apKey, aType, {acValue.Values[0], acValue.Values[1], acValue.Values[2]}};
            if (ValidTransform(record))
                Values.push_back(std::move(record));
        }
        return false;
    }
    std::vector<Transform> Values;
};

bool Female(const Actor& acActor)
{
    const auto* pNpc = Cast<TESNPC>(acActor.baseForm);
    return pNpc && (pNpc->actorData.actorBaseFlags & 1);
}

std::string OverlayNode(uint8_t aGroup, uint8_t aSlot)
{
    if (!s_overlay || aGroup >= 4 || aSlot >= 16 ||
        aSlot >= Invoke<11, uint32_t>(s_overlay, 0, static_cast<int>(aGroup)))
        return {};
    const char* pFormat = Invoke<12, const char*>(s_overlay, 0, static_cast<int>(aGroup));
    if (!pFormat)
        return {};
    const std::string_view format(pFormat);
    const auto marker = format.find('%');
    if (marker == format.npos || marker + 1 >= format.size() ||
        (format[marker + 1] != 'd' && format[marker + 1] != 'u') ||
        format.find('%', marker + 2) != format.npos)
        return {};
    char name[128]{};
    const int length = format[marker + 1] == 'u' ? std::snprintf(name, sizeof(name), pFormat, static_cast<unsigned int>(aSlot)) :
                                                  std::snprintf(name, sizeof(name), pFormat, static_cast<int>(aSlot));
    return length > 0 && static_cast<size_t>(length) < sizeof(name) ? name : "";
}

// Common exposed third-person skin slots. Texture key 9 indexes a texture set.
constexpr uint32_t kSkinSlots[]{1, 2, 4, 8, 0x80, 0x400};
constexpr uint8_t kSkinTextureCount = 8;
constexpr uint8_t kMaxSkinTextures = 48;
constexpr uint8_t kNoIndex = 255;

bool ValidSkinTexture(const SkinTexture& acValue)
{
    return std::ranges::find(kSkinSlots, acValue.Slot) != std::end(kSkinSlots) &&
        acValue.Index < kSkinTextureCount && ValidTexture(acValue.Texture);
}

bool GetNodeValue(TESObjectREFR* apActor, bool aFemale, const char* apNode, uint16_t aKey, uint8_t aIndex, GetValue& aValue)
{
    return Invoke<19, bool, TESObjectREFR*, bool, const char*, uint16_t, uint8_t, GetValue&>(
        s_override, apActor, aFemale, apNode, aKey, aIndex, aValue);
}

void AddNodeValue(TESObjectREFR* apActor, bool aFemale, const char* apNode, uint16_t aKey, uint8_t aIndex, SetValue& aValue)
{
    Invoke<18, void, TESObjectREFR*, bool, const char*, uint16_t, uint8_t, SetValue&>(
        s_override, apActor, aFemale, apNode, aKey, aIndex, aValue);
}

template <class T>
void Put(std::vector<uint8_t>& aBytes, const T& acValue)
{
    const auto* pValue = reinterpret_cast<const uint8_t*>(&acValue);
    aBytes.insert(aBytes.end(), pValue, pValue + sizeof(acValue));
}

void PutText(std::vector<uint8_t>& aBytes, const std::string& acText)
{
    Put(aBytes, static_cast<uint8_t>(acText.size()));
    aBytes.insert(aBytes.end(), acText.begin(), acText.end());
}

struct Reader
{
    const uint8_t* Data;
    size_t Size;

    template <class T> bool Read(T& aValue)
    {
        if (Size < sizeof(T))
            return false;
        std::memcpy(&aValue, Data, sizeof(T));
        Data += sizeof(T);
        Size -= sizeof(T);
        return true;
    }

    bool Text(std::string& aValue)
    {
        uint8_t length{};
        if (!Read(length) || length > Size)
            return false;
        aValue.assign(reinterpret_cast<const char*>(Data), length);
        Data += length;
        Size -= length;
        return true;
    }
};
}

void RaceMenuVisuals::Initialize(void* apInterfaceMap)
{
    const auto load = [apInterfaceMap](const char* apName, uint32_t aVersion)
    {
        auto* pInterface = Invoke<0, void*>(apInterfaceMap, apName);
        return pInterface && Invoke<1, uint32_t>(pInterface) == aVersion ? pInterface : nullptr;
    };
    s_overlay = load("Overlay", 2);
    s_override = load("Override", 2);
    s_transform = load("NiTransform", 3);
}

Snapshot RaceMenuVisuals::Capture(Actor& aActor)
{
    Snapshot snapshot;
    TESObjectREFR* pActor = &aActor;
    const bool female = Female(aActor);
    if (s_overlay && s_override)
        for (uint8_t group = 0; group < 4; ++group)
            for (uint8_t slot = 0; slot < 16; ++slot)
            {
                const auto node = OverlayNode(group, slot);
                if (node.empty())
                    break;
                GetValue texture, color, alpha;
                if (!GetNodeValue(pActor, female, node.c_str(), 9, 0, texture) || !ValidTexture(texture.Text))
                    continue;
                GetNodeValue(pActor, female, node.c_str(), 7, kNoIndex, color);
                GetNodeValue(pActor, female, node.c_str(), 8, kNoIndex, alpha);
                if (std::isfinite(alpha.Number) && alpha.Number >= 0.f && alpha.Number <= 1.f)
                    snapshot.Overlays.push_back({group, slot, std::move(texture.Text), static_cast<uint32_t>(color.Integer), alpha.Number});
            }

    if (s_override)
        for (const uint32_t slot : kSkinSlots)
            for (uint8_t index = 0; index < kSkinTextureCount; ++index)
            {
                GetValue texture;
                if (Invoke<30, bool, TESObjectREFR*, bool, bool, uint32_t, uint16_t, uint8_t, GetValue&>(
                        s_override, pActor, female, false, slot, uint16_t{9}, index, texture))
                {
                    SkinTexture row{slot, index, std::move(texture.Text)};
                    if (ValidSkinTexture(row))
                        snapshot.SkinTextures.push_back(std::move(row));
                }
            }

    if (s_transform)
    {
        NodeVisitor visitor;
        Invoke<23, void, TESObjectREFR*, bool, bool, NodeVisitor&>(s_transform, pActor, false, female, visitor);
        snapshot.Transforms = std::move(visitor.Values);
        std::ranges::sort(snapshot.Transforms, {}, [](const Transform& acValue)
        {
            return std::tie(acValue.Node, acValue.Key, acValue.Type);
        });
    }
    return snapshot;
}

void RaceMenuVisuals::Apply(Actor& aActor, const Snapshot& acPrevious, const Snapshot& acCurrent)
{
    TESObjectREFR* pActor = &aActor;
    const bool female = Female(aActor);
    if (s_overlay && s_override && (!acPrevious.Overlays.empty() || !acCurrent.Overlays.empty()))
    {
        if (!acCurrent.Overlays.empty() && !Invoke<3, bool>(s_overlay, pActor))
            Invoke<4, void>(s_overlay, pActor, false);
        std::vector<std::string> removed;
        for (const auto& overlay : acPrevious.Overlays)
        {
            const auto node = OverlayNode(overlay.Group, overlay.Slot);
            if (node.empty())
                continue;
            if (std::ranges::any_of(acCurrent.Overlays, [&](const auto& current)
                { return current.Group == overlay.Group && current.Slot == overlay.Slot; }))
                continue;
            // Restore the default texture/material for this slot. A zero-alpha
            // override is needed until RaceMenu next rebuilds its geometry.
            SetValue alpha;
            alpha.Kind = SetValue::Number;
            alpha.Value.Number = 0.f;
            Invoke<20, void>(s_override, pActor, female, node.c_str(), uint16_t{9}, uint8_t{0});
            Invoke<20, void>(s_override, pActor, female, node.c_str(), uint16_t{7}, kNoIndex);
            AddNodeValue(pActor, female, node.c_str(), 8, kNoIndex, alpha);
            removed.push_back(node);
        }
        for (const auto& overlay : acCurrent.Overlays)
        {
            const auto node = OverlayNode(overlay.Group, overlay.Slot);
            if (node.empty())
                continue;
            SetValue texture, color, alpha;
            texture.Kind = SetValue::Text;
            texture.Value.Text = overlay.Texture.c_str();
            color.Kind = SetValue::Integer;
            color.Value.Integer = static_cast<int32_t>(overlay.Color);
            alpha.Kind = SetValue::Number;
            alpha.Value.Number = overlay.Alpha;
            AddNodeValue(pActor, female, node.c_str(), 9, 0, texture);
            AddNodeValue(pActor, female, node.c_str(), 7, kNoIndex, color);
            AddNodeValue(pActor, female, node.c_str(), 8, kNoIndex, alpha);
        }
        Invoke<21, void>(s_override, pActor, true);
        for (const auto& node : removed)
            Invoke<20, void>(s_override, pActor, female, node.c_str(), uint16_t{8}, kNoIndex);
    }

    if (s_override && (!acPrevious.SkinTextures.empty() || !acCurrent.SkinTextures.empty()))
    {
        for (const auto& entry : acPrevious.SkinTextures)
        {
            if (std::ranges::any_of(acCurrent.SkinTextures, [&](const auto& current)
                { return entry.Slot == current.Slot && entry.Index == current.Index; }))
                continue;
            Invoke<31, void>(s_override, pActor, female, false, entry.Slot, uint16_t{9}, entry.Index);
        }
        for (const auto& entry : acCurrent.SkinTextures)
        {
            SetValue texture;
            texture.Kind = SetValue::Text;
            texture.Value.Text = entry.Texture.c_str();
            Invoke<29, void, TESObjectREFR*, bool, bool, uint32_t, uint16_t, uint8_t, SetValue&>(
                s_override, pActor, female, false, entry.Slot, uint16_t{9}, entry.Index, texture);
        }
        Invoke<32, void>(s_override, pActor, true);
    }

    if (!s_transform || (acPrevious.Transforms.empty() && acCurrent.Transforms.empty()))
        return;
    for (const auto& entry : acPrevious.Transforms)
    {
        const auto key = "SkyrimTogether/" + entry.Key;
        Invoke<19, bool>(s_transform, pActor, false, female, entry.Node.c_str(), key.c_str());
    }
    for (const auto& entry : acCurrent.Transforms)
    {
        const auto key = "SkyrimTogether/" + entry.Key;
        Triplet value{{entry.Values[0], entry.Values[1], entry.Values[2]}};
        if (entry.Type == Transform::Position)
            Invoke<7, void, TESObjectREFR*, bool, bool, const char*, const char*, Triplet&>(
                s_transform, pActor, false, female, entry.Node.c_str(), key.c_str(), value);
        else if (entry.Type == Transform::Rotation)
            Invoke<8, void, TESObjectREFR*, bool, bool, const char*, const char*, Triplet&>(
                s_transform, pActor, false, female, entry.Node.c_str(), key.c_str(), value);
        else if (entry.Type == Transform::Scale)
            Invoke<9, void>(s_transform, pActor, false, female, entry.Node.c_str(), key.c_str(), entry.Values[0]);
        else
            Invoke<10, void>(s_transform, pActor, false, female, entry.Node.c_str(), key.c_str(), static_cast<uint32_t>(entry.Values[0]));
    }
    Invoke<22, void>(s_transform, pActor);
}

std::vector<uint8_t> RaceMenuVisuals::Encode(const Snapshot& acSnapshot)
{
    std::vector<uint8_t> bytes(1);
    for (const auto& row : acSnapshot.Overlays)
    {
        if (bytes.size() + row.Texture.size() + 11 > 8 * 1024)
            break;
        Put(bytes, row.Group);
        Put(bytes, row.Slot);
        PutText(bytes, row.Texture);
        Put(bytes, row.Color);
        Put(bytes, row.Alpha);
        ++bytes[0];
    }
    const size_t countOffset = bytes.size();
    bytes.push_back(0);
    for (const auto& row : acSnapshot.Transforms)
    {
        if (bytes.size() - countOffset + row.Node.size() + row.Key.size() + 15 > 4 * 1024)
            break;
        PutText(bytes, row.Node);
        PutText(bytes, row.Key);
        Put(bytes, row.Type);
        Put(bytes, row.Values);
        ++bytes[countOffset];
    }
    const size_t skinOffset = bytes.size();
    bytes.push_back(0);
    for (const auto& row : acSnapshot.SkinTextures)
    {
        if (bytes.size() - skinOffset + row.Texture.size() + 6 > 4 * 1024)
            break;
        Put(bytes, row.Slot);
        Put(bytes, row.Index);
        PutText(bytes, row.Texture);
        ++bytes[skinOffset];
    }
    return bytes;
}

bool RaceMenuVisuals::Decode(const uint8_t* apData, size_t aSize, Snapshot& aSnapshot)
{
    Reader reader{apData, aSize};
    uint8_t count{};
    std::set<std::pair<uint8_t, uint8_t>> slots;
    if (!reader.Read(count) || count > 64)
        return false;
    for (unsigned int i = 0; i < count; ++i)
    {
        auto& row = aSnapshot.Overlays.emplace_back();
        if (!reader.Read(row.Group) || !reader.Read(row.Slot) || !reader.Text(row.Texture) ||
            !reader.Read(row.Color) || !reader.Read(row.Alpha) || row.Group >= 4 || row.Slot >= 16 ||
            !slots.emplace(row.Group, row.Slot).second || !ValidTexture(row.Texture) ||
            !(row.Alpha >= 0.f && row.Alpha <= 1.f))
            return false;
    }
    if (!reader.Read(count) || count > 96)
        return false;
    std::set<std::tuple<std::string, std::string, uint8_t>> transforms;
    for (unsigned int i = 0; i < count; ++i)
    {
        auto& row = aSnapshot.Transforms.emplace_back();
        if (!reader.Text(row.Node) || !reader.Text(row.Key) || !reader.Read(row.Type) ||
            !reader.Read(row.Values) || !ValidTransform(row) ||
            !transforms.emplace(row.Node, row.Key, row.Type).second)
            return false;
    }
    if (!reader.Read(count) || count > kMaxSkinTextures)
        return false;
    std::set<std::pair<uint32_t, uint8_t>> skinSlots;
    for (unsigned int i = 0; i < count; ++i)
    {
        auto& row = aSnapshot.SkinTextures.emplace_back();
        if (!reader.Read(row.Slot) || !reader.Read(row.Index) || !reader.Text(row.Texture) ||
            !ValidSkinTexture(row) || !skinSlots.emplace(row.Slot, row.Index).second)
            return false;
    }
    return !reader.Size;
}
