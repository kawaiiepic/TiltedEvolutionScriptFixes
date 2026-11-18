#pragma once
#include <cstdint>

namespace NativeCheckpointDriver
{
// Called from authenticated lobby handling on the game thread. Enabled by
// default on checkpoint servers; native signatures still need binary/gameplay validation.
bool TryRegister() noexcept;
void CheckForUnexpectedLoad() noexcept;
uint64_t NewGameGeneration() noexcept;
uint64_t LoadGeneration() noexcept;
}
