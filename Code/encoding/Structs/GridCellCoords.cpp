#include <Structs/GridCellCoords.h>
#include <TiltedCore/Serialization.hpp>
#include <cstdlib>

using TiltedPhoques::Serialization;

GridCellCoords::GridCellCoords()
{
    Reset();
}

GridCellCoords::GridCellCoords(int32_t aX, int32_t aY) noexcept
    : X(aX)
    , Y(aY)
{
}

bool GridCellCoords::operator==(const GridCellCoords& acRhs) const noexcept
{
    return X == acRhs.X && Y == acRhs.Y;
}

bool GridCellCoords::operator!=(const GridCellCoords& acRhs) const noexcept
{
    return !this->operator==(acRhs);
}

void GridCellCoords::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, X);
    Serialization::WriteVarInt(aWriter, Y);
}

void GridCellCoords::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    X = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Y = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
}

GridCellCoords GridCellCoords::CalculateGridCellCoords(const Vector3_NetQuantize& aCoords) noexcept
{
    return CalculateGridCellCoords(aCoords.x, aCoords.y);
}

GridCellCoords GridCellCoords::CalculateGridCellCoords(const float aX, const float aY) noexcept
{
    auto x = static_cast<int32_t>(floor(aX / 4096.f));
    auto y = static_cast<int32_t>(floor(aY / 4096.f));
    return GridCellCoords(x, y);
}

namespace
{
bool IsKnownGrid(const GridCellCoords& acCoords) noexcept
{
    constexpr int32_t kUnknown = std::numeric_limits<int32_t>::max();
    return acCoords.X != kUnknown && acCoords.Y != kUnknown;
}

bool IsWithinGridDistance(const GridCellCoords& acLeft, const GridCellCoords& acRight,
                          int32_t aDistance) noexcept
{
    if (!IsKnownGrid(acLeft) || !IsKnownGrid(acRight))
        return false;
    return std::abs(static_cast<int64_t>(acLeft.X) - acRight.X) <= aDistance &&
           std::abs(static_cast<int64_t>(acLeft.Y) - acRight.Y) <= aDistance;
}
}

bool GridCellCoords::AreGridCellsOverlapping(const GridCellCoords& aCoords1, const GridCellCoords& aCoords2) noexcept
{
    return IsWithinGridDistance(aCoords1, aCoords2, m_gridsToLoad - 1);
}

bool GridCellCoords::IsCellInGridCell(const GridCellCoords& aCell, const GridCellCoords& aGridCell, bool aIsDragon) noexcept
{
    const int32_t cGridSize = aIsDragon ? m_gridsToLoadIfDragon : m_gridsToLoad;
    return IsWithinGridDistance(aCell, aGridCell, cGridSize / 2);
}
