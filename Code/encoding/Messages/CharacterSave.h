#pragma once

#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include "Message.h"
#include <cstddef>
#include <array>
#include <cstdint>

// Bounded header and variable-length data. Preflight the exact packet size before
// extraction; no client-controlled allocations or filesystem names.
struct CharacterSavePayload
{
    static constexpr size_t ChunkSize = 60 * 1024;
    static constexpr size_t WindowSize = 8;
    static constexpr uint64_t MaxSize = 128ull * 1024 * 1024;
    static constexpr size_t HeaderSize = 1 + 2 + 4 * 8 + 32 + 32 + 2;
    static bool ValidPacket(const void* apData, size_t aSize) noexcept
    {
        if (aSize < HeaderSize || aSize > HeaderSize + ChunkSize) return false;
        const auto* bytes = static_cast<const uint8_t*>(apData);
        const size_t count = bytes[HeaderSize - 2] | (size_t(bytes[HeaderSize - 1]) << 8);
        return aSize == HeaderSize + count;
    }
    enum Operation : uint8_t { Select, BeginUpload, UploadChunk, Commit, DownloadChunk, Abort, UploadCopy, DownloadIfChanged, Roster, Create, SelectOwned, CompressedUploadChunk };
    enum Result : uint8_t { Ok, Disabled, Invalid, Busy, Stale, Missing, IoError, Incomplete, HashMismatch, Quota };
    uint8_t Action{};
    uint8_t Status{};
    uint64_t Revision{};
    uint64_t Offset{};
    uint64_t EssSize{};
    uint64_t CoSaveSize{};
    uint16_t Count{};
    std::array<uint8_t, 32> Key{}; // bearer capability; sent only for Select
    std::array<uint8_t, 32> Digest{};
    std::array<uint8_t, ChunkSize> Data{};

    void Write(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
    {
        aWriter.WriteBits(Action, 8);
        aWriter.WriteBits(Status, 8);
        aWriter.WriteBits(Revision, 64);
        aWriter.WriteBits(Offset, 64);
        aWriter.WriteBits(EssSize, 64);
        aWriter.WriteBits(CoSaveSize, 64);
        aWriter.WriteBytes(Key.data(), Key.size());
        aWriter.WriteBytes(Digest.data(), Digest.size());
        aWriter.WriteBits(Count, 16);
        if (Count <= ChunkSize) aWriter.WriteBytes(Data.data(), Count);
    }
    void Read(TiltedPhoques::Buffer::Reader& aReader) noexcept
    {
        uint64_t value{};
        aReader.ReadBits(value, 8); Action = static_cast<uint8_t>(value);
        aReader.ReadBits(value, 8); Status = static_cast<uint8_t>(value);
        aReader.ReadBits(Revision, 64);
        aReader.ReadBits(Offset, 64);
        aReader.ReadBits(EssSize, 64);
        aReader.ReadBits(CoSaveSize, 64);
        aReader.ReadBytes(Key.data(), Key.size());
        aReader.ReadBytes(Digest.data(), Digest.size());
        aReader.ReadBits(value, 16); Count = static_cast<uint16_t>(value);
        if (Count <= ChunkSize) aReader.ReadBytes(Data.data(), Count);
    }
};

struct RequestCharacterSave final : ClientMessage, CharacterSavePayload
{
    static constexpr ClientOpcode Opcode = kRequestCharacterSave;
    RequestCharacterSave() : ClientMessage(Opcode) {}
    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override { Write(aWriter); }
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override { Read(aReader); }
};

struct NotifyCharacterSave final : ServerMessage, CharacterSavePayload
{
    static constexpr ServerOpcode Opcode = kNotifyCharacterSave;
    NotifyCharacterSave() : ServerMessage(Opcode) {}
    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override { Write(aWriter); }
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override { Read(aReader); }
};
