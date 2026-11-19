#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include "../server/Services/DialogueLocation.h"
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
