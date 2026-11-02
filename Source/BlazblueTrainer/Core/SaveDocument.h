#pragma once

#include <lz4frame.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Trainer
{
    using Bytes = std::vector<std::uint8_t>;

    struct ProtoField
    {
        std::uint32_t Number{};
        std::uint8_t WireType{};
        std::uint64_t Integer{};
        Bytes Data;
        Bytes Raw;

        static ProtoField Varint(std::uint32_t Number, std::uint64_t Value);
        static ProtoField Blob(std::uint32_t Number, std::span<const std::uint8_t> Value);
        static ProtoField Double(std::uint32_t Number, double Value);
    };

    class ProtoMessage final
    {
      public:
        static ProtoMessage Parse(std::span<const std::uint8_t> Data);
        Bytes Serialize() const;
        const std::vector<ProtoField> &Fields() const
        {
            return Members;
        }
        std::vector<ProtoField> Select(std::uint32_t Number) const;
        std::uint64_t Integer(std::uint32_t Number, std::uint64_t Default = 0) const;
        Bytes Blob(std::uint32_t Number) const;
        std::string String(std::uint32_t Number) const;
        double Double(std::uint32_t Number, double Default = 0) const;
        std::vector<std::uint32_t> PackedIntegers(std::uint32_t Number) const;
        void Replace(std::uint32_t Number, const std::vector<ProtoField> &Fields);
        void Add(ProtoField Field);

      private:
        const ProtoField *Scalar(std::uint32_t Number, std::uint8_t WireType) const;
        std::vector<ProtoField> Members;
    };

    class SaveDocument final
    {
      public:
        static SaveDocument Decode(std::span<const std::uint8_t> Compressed);
        Bytes Encode() const;
        Bytes Model(const std::string &Name) const;
        void ReplaceModel(const std::string &Name, const Bytes &Payload);
        const Bytes &Original() const
        {
            return OriginalBytes;
        }

      private:
        std::size_t ModelIndex(const std::string &Name) const;
        ProtoMessage Root;
        Bytes OriginalBytes;
        Bytes OriginalPayload;
        LZ4F_frameInfo_t FrameInfo{};
    };
} // namespace Trainer
