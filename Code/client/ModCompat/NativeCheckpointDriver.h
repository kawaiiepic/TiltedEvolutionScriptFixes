#pragma once

namespace NativeCheckpointDriver
{
// Called from authenticated lobby handling on the game thread. Enabled by
// default on checkpoint servers; native signatures still need binary/gameplay validation.
bool TryRegister() noexcept;
void CheckForUnexpectedLoad() noexcept;
}
