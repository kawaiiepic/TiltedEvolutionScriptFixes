#pragma once

namespace NativeCheckpointDriver
{
// Called from authenticated lobby handling on the game thread. Off by
// default; public source signatures still need binary/gameplay validation.
bool TryRegister() noexcept;
void CheckForUnexpectedLoad() noexcept;
}
