#include <Structs/SceneState.h>
#include <algorithm>

void SceneState::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Id.Serialize(aWriter);
    Quest.Serialize(aWriter);
    aWriter.WriteBits(Phase, 32);
    aWriter.WriteBits(Active ? 1 : 0, 1);
    const uint32_t cCount = static_cast<uint32_t>(std::min<size_t>(CompletedActions.size(), 255));
    aWriter.WriteBits(cCount, 8);
    for (uint32_t i = 0; i < cCount; ++i)
        aWriter.WriteBits(CompletedActions[i], 32);

    const uint32_t cAliasCount = static_cast<uint32_t>(std::min<size_t>(ReferenceAliases.size(), 255));
    aWriter.WriteBits(cAliasCount, 8);
    for (uint32_t i = 0; i < cAliasCount; ++i)
    {
        aWriter.WriteBits(ReferenceAliases[i].AliasId, 32);
        ReferenceAliases[i].Reference.Serialize(aWriter);
    }
}

void SceneState::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    Id.Deserialize(aReader);
    Quest.Deserialize(aReader);
    uint64_t value{};
    aReader.ReadBits(value, 32);
    Phase = static_cast<uint32_t>(value);
    aReader.ReadBits(value, 1);
    Active = value != 0;
    aReader.ReadBits(value, 8);
    CompletedActions.resize(static_cast<size_t>(value));
    for (auto& action : CompletedActions)
    {
        aReader.ReadBits(value, 32);
        action = static_cast<uint32_t>(value);
    }

    aReader.ReadBits(value, 8);
    ReferenceAliases.resize(static_cast<size_t>(value));
    for (auto& alias : ReferenceAliases)
    {
        aReader.ReadBits(value, 32);
        alias.AliasId = static_cast<uint32_t>(value);
        alias.Reference.Deserialize(aReader);
    }
}
