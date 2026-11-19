#include "MenuTopicManager.h"

#include <Services/OverlayService.h>
#include <World.h>
#include <TESObjectREFR.h>
#include <cstring>

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

bool MenuTopicManager::ChooseSharedOption(uint32_t aIndex, const char* apExpectedText) noexcept
{
    if (!menuOpen || !pOptions || !RealPlayDialogueOption || !apExpectedText || !*apExpectedText || aIndex >= 64)
        return false;
    uint32_t index = 0;
    for (auto* option : *pOptions)
    {
        if (index++ != aIndex) continue;
        if (!option || !option->text || std::strcmp(option->text, apExpectedText) != 0)
            return false;
        // Same ownership gate and engine choice routine as a native menu click.
        if (!World::Get().GetOverlayService().CanSelectDialogue(option->text))
            return false;
        return TiltedPhoques::ThisCall(RealPlayDialogueOption, this, static_cast<int32_t>(aIndex));
    }
    return false;
}

static TiltedPhoques::Initializer s_menuTopicHooks(
    []()
    {
        POINTER_SKYRIMSE(TPlayDialogueOption, s_playDialogueOption, 35269);

        RealPlayDialogueOption = s_playDialogueOption.Get();

        TP_HOOK(&RealPlayDialogueOption, HookPlayDialogueOption);
    });
