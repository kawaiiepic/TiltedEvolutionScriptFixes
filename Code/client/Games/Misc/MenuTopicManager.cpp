#include "MenuTopicManager.h"

#include <Services/OverlayService.h>
#include <World.h>
#include <TESObjectREFR.h>

TP_THIS_FUNCTION(TPlayDialogueOption, bool, MenuTopicManager, int32_t aIndex);
static TPlayDialogueOption* RealPlayDialogueOption = nullptr;

MenuTopicManager* MenuTopicManager::Get() noexcept
{
    POINTER_SKYRIMSE(MenuTopicManager*, s_singleton, 401099);
    return *s_singleton.Get();
}

bool MenuTopicManager::IsPlayerDialogueSpeaker(const TESObjectREFR* apSpeaker) noexcept
{
    if (!apSpeaker)
        return false;

    const auto* pManager = Get();
    // lastSpeaker survives menu closure; using it here would also capture
    // subsequent ambient speech from a remotely owned NPC.
    return pManager && pManager->IsCurrentSpeaker(apSpeaker->GetHandle());
}

bool TP_MAKE_THISCALL(HookPlayDialogueOption, MenuTopicManager, int32_t aIndex)
{
    if (apThis->menuOpen && World::Get().GetTransport().IsOnline())
    {
        const char* pText = nullptr;
        if (apThis->pOptions)
        {
            int i = 0;
            for (auto* pOption : *apThis->pOptions)
            {
                if (i++ == aIndex)
                {
                    if (pOption)
                        pText = pOption->text;
                    break;
                }
            }
        }

        // Never invoke the native choice routine on a suggest-only client.
        if (!World::Get().GetOverlayService().CanSelectDialogue(pText))
            return false;
    }

    return TiltedPhoques::ThisCall(RealPlayDialogueOption, apThis, aIndex);
}

static TiltedPhoques::Initializer s_menuTopicHooks(
    []()
    {
        POINTER_SKYRIMSE(TPlayDialogueOption, s_playDialogueOption, 35269);

        RealPlayDialogueOption = s_playDialogueOption.Get();

        TP_HOOK(&RealPlayDialogueOption, HookPlayDialogueOption);
    });
