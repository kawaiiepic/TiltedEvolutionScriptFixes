#pragma once

#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/ViewBuffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include "Message.h"
#include <cstddef>
#include <array>
#include <cstdint>

// Bounded, direction-aware wire format. Preflight the exact packet size before
// extraction; no client-controlled allocations or filesystem names.
struct CharacterSavePayload
{
    static constexpr size_t ChunkSize = 32768;
    static constexpr size_t WindowSize = 8;
    static constexpr uint64_t MaxSize = 128ull * 1024 * 1024;
    static constexpr size_t HeaderWireSize = 1 + 2 + 4 * 8 + 2 + 32 + 32;
    static constexpr size_t WireSize = HeaderWireSize + ChunkSize;
    enum Operation : uint8_t { Select, BeginUpload, UploadChunk, Commit, DownloadChunk, Abort, CompressedUploadChunk, ReuseChunk };
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

    static bool HasBody(uint8_t aAction, bool aRequest) noexcept
    {
        return aRequest ? aAction == UploadChunk || aAction == CompressedUploadChunk : aAction == DownloadChunk;
    }
    static bool ValidWireSize(const void* apData, uint32_t aSize, bool aRequest) noexcept
    {
        if (aSize < HeaderWireSize || aSize > WireSize) return false;
        TiltedPhoques::ViewBuffer buffer(const_cast<uint8_t*>(static_cast<const uint8_t*>(apData)), aSize);
        TiltedPhoques::Buffer::Reader reader(&buffer);
        uint64_t value{}, action{}, count{};
        reader.ReadBits(value, 8);
        reader.ReadBits(action, 8);
        reader.ReadBits(value, 8);
        for (unsigned i = 0; i < 4; ++i) reader.ReadBits(value, 64);
        reader.ReadBits(count, 16);
        return action <= ReuseChunk && count <= ChunkSize &&
            aSize == HeaderWireSize + (HasBody(static_cast<uint8_t>(action), aRequest) ? count : 0);
    }

    void Write(TiltedPhoques::Buffer::Writer& aWriter, bool aRequest) const noexcept
    {
        aWriter.WriteBits(Action, 8);
        aWriter.WriteBits(Status, 8);
        aWriter.WriteBits(Revision, 64);
        aWriter.WriteBits(Offset, 64);
        aWriter.WriteBits(EssSize, 64);
        aWriter.WriteBits(CoSaveSize, 64);
        aWriter.WriteBits(Count, 16);
        for (auto byte : Key) aWriter.WriteBits(byte, 8);
        for (auto byte : Digest) aWriter.WriteBits(byte, 8);
        if (HasBody(Action, aRequest))
            for (size_t i = 0; i < Count && i < Data.size(); ++i) aWriter.WriteBits(Data[i], 8);
    }
    void Read(TiltedPhoques::Buffer::Reader& aReader, bool aRequest) noexcept
    {
        uint64_t value{};
        aReader.ReadBits(value, 8); Action = static_cast<uint8_t>(value);
        aReader.ReadBits(value, 8); Status = static_cast<uint8_t>(value);
        aReader.ReadBits(Revision, 64);
        aReader.ReadBits(Offset, 64);
        aReader.ReadBits(EssSize, 64);
        aReader.ReadBits(CoSaveSize, 64);
        aReader.ReadBits(value, 16); Count = static_cast<uint16_t>(value);
        for (auto& byte : Key) { aReader.ReadBits(value, 8); byte = static_cast<uint8_t>(value); }
        for (auto& byte : Digest) { aReader.ReadBits(value, 8); byte = static_cast<uint8_t>(value); }
        if (HasBody(Action, aRequest))
            for (size_t i = 0; i < Count && i < Data.size(); ++i) { aReader.ReadBits(value, 8); Data[i] = static_cast<uint8_t>(value); }
    }
};

struct RequestCharacterSave final : ClientMessage, CharacterSavePayload
{
    static constexpr ClientOpcode Opcode = kRequestCharacterSave;
    RequestCharacterSave() : ClientMessage(Opcode) {}
    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override { Write(aWriter, true); }
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override { Read(aReader, true); }
};

struct NotifyCharacterSave final : ServerMessage, CharacterSavePayload
{
    static constexpr ServerOpcode Opcode = kNotifyCharacterSave;
    NotifyCharacterSave() : ServerMessage(Opcode) {}
    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override { Write(aWriter, false); }
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override { Read(aReader, false); }
};
