#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include "../server/Services/DialogueLocation.h"
#include <Structs/GridCellCoords.h>
#include <catch2/catch.hpp>

TEST_CASE("Public dialogue separates exterior worldspaces and interior cells", "[dialogue]")
{
    const GameId tamriel{0, 0x3C};
    const GameId modWorldSpace{10, 0x3C};
    const GameId riverwood{0, 0x2000};
    const GameId riften{0, 0x2001};
    const GameId inn{0, 0x3000};
    const GameId modInn{10, 0x3000};
    using DialogueLocation::IsCompatible;

    CHECK(IsCompatible(riverwood, tamriel, riften, tamriel));
    CHECK_FALSE(IsCompatible(riverwood, tamriel, riften, modWorldSpace));
    CHECK(IsCompatible(inn, {}, inn, {}));
    CHECK_FALSE(IsCompatible(inn, {}, modInn, {}));
    CHECK_FALSE(IsCompatible(inn, {}, riverwood, tamriel));
    CHECK_FALSE(IsCompatible(riverwood, tamriel, inn, {}));
    CHECK_FALSE(IsCompatible({}, {}, {}, {}));
    CHECK_FALSE(IsCompatible(riverwood, tamriel, {}, tamriel));
}


TEST_CASE("Unknown exterior coordinates do not make actors nearby", "[dialogue]")
{
    const GridCellCoords unknown;
    const GridCellCoords origin{0, 0};
    const GridCellCoords minimum{std::numeric_limits<int32_t>::min(), 0};
    const GridCellCoords maximum{std::numeric_limits<int32_t>::max() - 1, 0};

    CHECK_FALSE(GridCellCoords::IsCellInGridCell(unknown, unknown, false));
    CHECK_FALSE(GridCellCoords::IsCellInGridCell(origin, unknown, false));
    CHECK_FALSE(GridCellCoords::IsCellInGridCell(minimum, maximum, false));
    CHECK_FALSE(GridCellCoords::AreGridCellsOverlapping(unknown, unknown));
    CHECK_FALSE(GridCellCoords::AreGridCellsOverlapping(minimum, maximum));
    CHECK(GridCellCoords::IsCellInGridCell({2, -2}, origin, false));
    CHECK_FALSE(GridCellCoords::IsCellInGridCell({3, 0}, origin, false));
    CHECK(GridCellCoords::IsCellInGridCell({10, 0}, origin, true));
    CHECK(GridCellCoords::AreGridCellsOverlapping({4, 0}, origin));
    CHECK_FALSE(GridCellCoords::AreGridCellsOverlapping({5, 0}, origin));
}
