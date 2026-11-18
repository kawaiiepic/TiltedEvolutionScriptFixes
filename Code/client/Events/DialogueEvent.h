#pragma once

/**
 * @brief Dispatched whenever an actor talks
 */
struct DialogueEvent
{
    DialogueEvent(uint32_t aActorID, String aVoiceFile, uint32_t aSceneID, uint32_t aScenePhase, bool aPlayerDialogue)
        : ActorID(aActorID)
        , VoiceFile(aVoiceFile)
        , SceneID(aSceneID)
        , PlayerDialogue(aPlayerDialogue)
        , ScenePhase(aScenePhase)
    {
    }

    uint32_t ActorID;
    String VoiceFile;
    uint32_t SceneID; // Scene the line is spoken in, 0 for player dialogue menu lines and lines outside scenes
    bool PlayerDialogue;
    uint32_t ScenePhase; // The scene's current phase when the line started
};
