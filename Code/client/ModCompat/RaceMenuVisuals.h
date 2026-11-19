#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct Actor;

namespace RaceMenuVisuals
{
struct Overlay
{
    uint8_t Group{};
    uint8_t Slot{};
    std::string Texture;
    uint32_t Color{0xFFFFFF};
    float Alpha{1.f};
    bool operator==(const Overlay&) const = default;
};

struct Transform
{
    enum Kind : uint8_t { Position, Rotation, Scale, ScaleMode };
    std::string Node;
    std::string Key;
    uint8_t Type{};
    float Values[3]{};
    bool operator==(const Transform&) const = default;
};

struct Snapshot
{
    std::vector<Overlay> Overlays;
    std::vector<Transform> Transforms;
    bool operator==(const Snapshot&) const = default;
};

void Initialize(void* apInterfaceMap);
Snapshot Capture(Actor& aActor);
void Apply(Actor& aActor, const Snapshot& acPrevious, const Snapshot& acCurrent);
std::vector<uint8_t> Encode(const Snapshot& acSnapshot);
bool Decode(const uint8_t* apData, size_t aSize, Snapshot& aSnapshot);
}
