#pragma once

struct NativeFunction;

namespace ImmersiveInteractions
{
void RegisterMessaging() noexcept;
void BindPlayIdle(NativeFunction& aFunction) noexcept;
}
