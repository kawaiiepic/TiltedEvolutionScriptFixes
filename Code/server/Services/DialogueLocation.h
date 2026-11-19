#pragma once

#include <Structs/GameId.h>

namespace DialogueLocation
{
inline bool IsCompatible(const GameId& acSpeakerCell, const GameId& acSpeakerWorldSpace,
                         const GameId& acListenerCell, const GameId& acListenerWorldSpace) noexcept
{
    if (!acSpeakerCell || !acListenerCell)
        return false;
    if (acSpeakerWorldSpace || acListenerWorldSpace)
        return acSpeakerWorldSpace == acListenerWorldSpace;
    return acSpeakerCell == acListenerCell;
}
}
