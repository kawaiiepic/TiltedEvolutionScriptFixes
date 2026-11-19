#include <TiltedOnlinePCH.h>

#include <Interface/IMenu.h>
#include <Interface/UI.h>
#include <Messages/SendChatMessageRequest.h>
#include <Utils.h>
#include <VersionDb.h>
#include <World.h>

#include <cmath>
#include <cstring>

namespace
{
using Accept = void(IMenu*, FxDelegateHandler::CallbackProcessor*);
using ProcessMessage = UI_MESSAGE_RESULTS(IMenu*, UIMessage&);
using AddMessage = void(void*, const BSFixedString&, UIMessage::UI_MESSAGE_TYPE, void*);
using Callback = FxDelegateHandler::CallbackFn;

Accept* s_accept{};
ProcessMessage* s_processMessage{};
AddMessage* s_addMessage{};
Callback* s_confirm{};
void** s_messageQueue{};
const void* s_messageDataVtable{};

IMenu* s_openMenu{};
bool s_sleeping{};
bool s_knownMode{};
bool s_confirmationHandled{};

struct ScaleformValue
{
    void* ObjectInterface;
    uint32_t Type;
    uint32_t Padding;
    double Number;
};
static_assert(sizeof(ScaleformValue) == 0x18);

struct DelegateArgs
{
    ScaleformValue ResponseId;
    IMenu* Handler;
    void* Movie;
    const ScaleformValue* Arguments;
    uint32_t ArgumentCount;
};
static_assert(offsetof(DelegateArgs, Arguments) == 0x28);
static_assert(offsetof(DelegateArgs, ArgumentCount) == 0x30);

void Confirm(const void* apArgs)
{
    if (!entt::locator<World>::has_value() || !World::Get().GetTransport().IsOnline())
    {
        s_confirm(apArgs);
        return;
    }

    const auto& args = *static_cast<const DelegateArgs*>(apArgs);
    if (s_confirmationHandled || !s_openMenu || args.Handler != s_openMenu)
        return;

    const auto* pHours = args.ArgumentCount == 1 ? args.Arguments : nullptr;
    const bool validHours = pHours && pHours->Type == 3 && pHours->Number >= 1.0 &&
                            pHours->Number <= 24.0 && std::floor(pHours->Number) == pHours->Number;
    auto* pQueue = *s_messageQueue;
    if (!pQueue)
        return;

    // The native callback starts the wait immediately. Close the selection
    // menu through the UI queue, then submit only the shared calendar vote.
    s_confirmationHandled = true;
    static const BSFixedString s_menuName("Sleep/Wait Menu");
    s_addMessage(pQueue, s_menuName, UIMessage::kHide, nullptr);

    const bool sleeping = s_sleeping;
    const bool knownMode = s_knownMode;
    const unsigned int hours = validHours ? static_cast<unsigned int>(pHours->Number) : 0;
    World::Get().GetRunner().Queue([sleeping, knownMode, hours]()
    {
        auto& transport = World::Get().GetTransport();
        if (!transport.IsOnline())
            return;
        if (!knownMode)
        {
            Utils::ShowHudMessage("Could not read the Sleep/Wait menu. Use /rest to start a group vote.");
            return;
        }
        if (!hours)
        {
            Utils::ShowHudMessage("Group rest requires a selection of 1 to 24 hours.");
            return;
        }

        SendChatMessageRequest request{};
        request.MessageType = kGlobalChat;
        request.ChatMessage = fmt::format("/rest {} {}", sleeping ? "sleep" : "wait", hours).c_str();
        if (!transport.Send(request))
            Utils::ShowHudMessage("Could not send the group rest vote.");
    });
}

struct CallbackProcessor final : FxDelegateHandler::CallbackProcessor
{
    explicit CallbackProcessor(FxDelegateHandler::CallbackProcessor& aOriginal) : Original(aOriginal) {}

    void Process(const void* apName, Callback* apCallback) override
    {
        // Scaleform GString stores two heap flags in its descriptor pointer.
        const auto descriptor = *static_cast<const uintptr_t*>(apName) & ~uintptr_t{3};
        const auto* pName = descriptor ? reinterpret_cast<const char*>(descriptor + 0xC) : "";
        if (std::strcmp(pName, "OK") == 0)
        {
            s_confirm = apCallback;
            apCallback = &Confirm;
        }
        Original.Process(apName, apCallback);
    }

    FxDelegateHandler::CallbackProcessor& Original;
};

void AcceptCallbacks(IMenu* apMenu, FxDelegateHandler::CallbackProcessor* apProcessor)
{
    CallbackProcessor processor(*apProcessor);
    s_accept(apMenu, &processor);
}

UI_MESSAGE_RESULTS ProcessSleepWaitMessage(IMenu* apMenu, UIMessage& aMessage)
{
    if (aMessage.eType == UIMessage::kShow)
    {
        s_openMenu = apMenu;
        s_confirmationHandled = false;
        // The sleep variant sets BSUIMessageData::data.b in kShow.
        const auto* pData = static_cast<const uint8_t*>(aMessage.data);
        s_knownMode = !pData || *reinterpret_cast<const void* const*>(pData) == s_messageDataVtable;
        s_sleeping = s_knownMode && pData && pData[0x20] != 0;
    }
    const auto result = s_processMessage(apMenu, aMessage);
    if (apMenu == s_openMenu && result == UI_MESSAGE_RESULTS::kHandled &&
        (aMessage.eType == UIMessage::kHide || aMessage.eType == UIMessage::kForceHide))
        s_openMenu = nullptr;
    return result;
}

static TiltedPhoques::Initializer s_sleepWaitHooks(
    []()
    {
        const auto& db = VersionDb::Get();
        auto** pVtable = static_cast<void**>(db.FindAddressById(215936)); // SleepWaitMenu
        s_messageDataVtable = db.FindAddressById(207216); // BSUIMessageData
        s_messageQueue = static_cast<void**>(db.FindAddressById(400445)); // UIMessageQueue singleton
        s_addMessage = reinterpret_cast<AddMessage*>(db.FindAddressById(13631));

        constexpr size_t kAccept = 1;
        constexpr size_t kProcessMessage = 4;
        DWORD protection{};
        if (!pVtable || !s_messageDataVtable || !s_messageQueue || !s_addMessage ||
            !VirtualProtect(pVtable, (kProcessMessage + 1) * sizeof(void*), PAGE_READWRITE, &protection))
        {
            spdlog::warn("Native Sleep/Wait vote hook unavailable. Use /rest to start a group vote.");
            return;
        }

        s_accept = reinterpret_cast<Accept*>(pVtable[kAccept]);
        s_processMessage = reinterpret_cast<ProcessMessage*>(pVtable[kProcessMessage]);
        pVtable[kAccept] = reinterpret_cast<void*>(&AcceptCallbacks);
        pVtable[kProcessMessage] = reinterpret_cast<void*>(&ProcessSleepWaitMessage);
        DWORD ignored{};
        VirtualProtect(pVtable, (kProcessMessage + 1) * sizeof(void*), protection, &ignored);
    });
}
