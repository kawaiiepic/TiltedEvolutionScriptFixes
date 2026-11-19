#include <TiltedOnlinePCH.h>

#include <ModCompat/ImmersiveInteractions.h>
#include <Services/PluginMessagingService.h>
#include <Forms/TESIdleForm.h>
#include <Games/Overrides.h>
#include <Games/TES.h>
#include <Misc/NativeFunction.h>
#include <PlayerCharacter.h>
#include <World.h>

#include <cstring>

extern thread_local bool g_forceAnimation;

namespace
{
constexpr char kChannel[] = "skyrimtogether.immersive-interactions.idle.v1";
constexpr char kPlugin[] = "ImmersiveInteractions.esp";
using PlayIdle = bool (*)(BSScript::IVirtualMachine*, uint32_t, const Actor*, TESIdleForm*);
PlayIdle s_playIdle{};

struct IdleMessage
{
    uint32_t ModId;
    uint32_t BaseId;
};
static_assert(sizeof(IdleMessage) == 8);

bool IsImmersiveInteraction(uint32_t aFormId) noexcept
{
    const auto* pModManager = ModManager::Get();
    const auto* pMod = pModManager ? pModManager->GetByName(kPlugin) : nullptr;
    return pMod && aFormId && pMod->GetFormId(aFormId) == aFormId;
}

void ReceiveIdle(uint32_t aSender, const void* apData, size_t aSize, void*)
{
    if (!apData || aSize != sizeof(IdleMessage) || !s_playIdle)
        return;

    IdleMessage message{};
    std::memcpy(&message, apData, sizeof(message));
    const auto& mods = World::Get().GetModSystem();
    const uint32_t idleId = mods.GetGameId(message.ModId, message.BaseId);
    if (!IsImmersiveInteraction(idleId))
        return;

    auto* pIdle = Cast<TESIdleForm>(TESForm::GetById(idleId));
    auto* pActor = Cast<Actor>(TESForm::GetById(PluginMessagingService::GetInterface().ResolvePlayerFormId(aSender)));
    if (!pIdle || !pActor || pActor == PlayerCharacter::Get() || pActor->IsDead())
        return;

    ScopedAnimationOverride override;
    const bool previousForce = std::exchange(g_forceAnimation, true);
    pActor->PlayIdle(pIdle);
    g_forceAnimation = previousForce;
}

bool PlayInteractionIdle(BSScript::IVirtualMachine* apVM, uint32_t aStack, const Actor* apActor, TESIdleForm* apIdle)
{
    if (apActor != PlayerCharacter::Get() || !apIdle || !IsImmersiveInteraction(apIdle->formID))
        return s_playIdle(apVM, aStack, apActor, apIdle);

    // This channel carries the Papyrus idle, so suppress the ordinary action relay
    // during this native call. Vanilla processing and scripts still run locally.
    ScopedAnimationOverride override;
    const uint32_t idleId = apIdle->formID;
    const bool played = s_playIdle(apVM, aStack, apActor, apIdle);
    if (played)
        World::Get().QueueCheckpointTask([idleId]()
        {
            const auto& mods = World::Get().GetModSystem();
            GameId id{};
            if (mods.GetServerModId(idleId, id) && mods.GetGameId(id) == idleId)
            {
                const IdleMessage message{id.ModId, id.BaseId};
                PluginMessagingService::GetInterface().Send(kChannel, SkyrimTogether::Nearby, &message, sizeof(message));
            }
        });
    return played;
}
}

void ImmersiveInteractions::RegisterMessaging() noexcept
{
    PluginMessagingService::GetInterface().Register(kChannel, &ReceiveIdle, nullptr);
}

void ImmersiveInteractions::BindPlayIdle(NativeFunction& aFunction) noexcept
{
    const auto* pType = aFunction.typeName.AsAscii();
    const auto* pName = aFunction.functionName.AsAscii();
    if (!pType || !pName || std::strcmp(pType, "Actor") || std::strcmp(pName, "PlayIdle") ||
        !aFunction.functionAddress || aFunction.functionAddress == reinterpret_cast<void*>(&PlayInteractionIdle))
        return;

    s_playIdle = reinterpret_cast<PlayIdle>(aFunction.functionAddress);
    aFunction.functionAddress = reinterpret_cast<void*>(&PlayInteractionIdle);
}
