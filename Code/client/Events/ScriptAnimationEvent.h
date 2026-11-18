#pragma once

struct ScriptAnimationEvent
{
    ScriptAnimationEvent(uint32_t aFormId, const char* apName)
        : FormID(aFormId)
        , EventName(apName)
    {
    }

    uint32_t FormID;
    String EventName;
};
