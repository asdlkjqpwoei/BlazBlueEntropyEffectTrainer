#include "SaveDocument.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace Trainer
{
	namespace
	{
		constexpr std::size_t MaxSaveBytes = 64 * 1024 * 1024;

		void EncodeInteger(Bytes &Output, std::uint64_t Value)
		{
			do
			{
				auto Byte = static_cast<std::uint8_t>(Value & 0x7f);
				Value >>= 7;
				Output.push_back(static_cast<std::uint8_t>(Byte | (Value ? 0x80 : 0)));
			} while (Value);
		}

		std::uint64_t DecodeInteger(std::span<const std::uint8_t> Data, std::size_t &Position)
		{
			std::uint64_t Value = 0;
			for (unsigned Index = 0; Index < 10; ++Index)
			{
				if (Position >= Data.size())
					throw std::runtime_error("Truncated protobuf integer.");
				const auto Byte = Data[Position++];
				if (Index == 9 && Byte > 1)
					throw std::runtime_error("Protobuf integer overflow.");
				Value |= static_cast<std::uint64_t>(Byte & 0x7f) << (Index * 7);
				if (!(Byte & 0x80))
					return Value;
			}
			throw std::runtime_error("Protobuf integer overflow.");
		}

		void CheckNumber(std::uint32_t Number)
		{
			if (Number == 0 || Number >= (1u << 29))
				throw std::runtime_error("Invalid protobuf field number.");
		}

		void CheckLz4(std::size_t Result)
		{
			if (LZ4F_isError(Result))
				throw std::runtime_error(std::string("Invalid LZ4 save: ") + LZ4F_getErrorName(Result));
		}

		struct Lz4Reader
		{
			Lz4Reader()
			{
				CheckLz4(LZ4F_createDecompressionContext(&Handle, LZ4F_VERSION));
			}
			~Lz4Reader()
			{
				LZ4F_freeDecompressionContext(Handle);
			}
			LZ4F_dctx *Handle{};
		};
	} // namespace

	ProtoField ProtoField::Varint(std::uint32_t Number, std::uint64_t Value)
	{
		CheckNumber(Number);
		ProtoField Result{Number, 0, Value, {}, {}};
		EncodeInteger(Result.Raw, static_cast<std::uint64_t>(Number) << 3);
		EncodeInteger(Result.Raw, Value);
		return Result;
	}

	ProtoField ProtoField::Blob(std::uint32_t Number, std::span<const std::uint8_t> Value)
	{
		CheckNumber(Number);
		ProtoField Result{Number, 2, 0, Bytes(Value.begin(), Value.end()), {}};
		EncodeInteger(Result.Raw, (static_cast<std::uint64_t>(Number) << 3) | 2);
		EncodeInteger(Result.Raw, Value.size());
		Result.Raw.insert(Result.Raw.end(), Value.begin(), Value.end());
		return Result;
	}

	ProtoField ProtoField::Double(std::uint32_t Number, double Value)
	{
		CheckNumber(Number);
		ProtoField Result{Number, 1, 0, {}, {}};
		const auto Bits = std::bit_cast<std::uint64_t>(Value);
		for (unsigned Index = 0; Index < 8; ++Index)
			Result.Data.push_back(static_cast<std::uint8_t>(Bits >> (Index * 8)));
		EncodeInteger(Result.Raw, (static_cast<std::uint64_t>(Number) << 3) | 1);
		Result.Raw.insert(Result.Raw.end(), Result.Data.begin(), Result.Data.end());
		return Result;
	}

	ProtoMessage ProtoMessage::Parse(std::span<const std::uint8_t> Data)
	{
		if (Data.size() > MaxSaveBytes)
			throw std::runtime_error("Save message exceeds the bounded size limit.");
		ProtoMessage Result;
		std::size_t Position = 0;
		while (Position < Data.size())
		{
			const auto Begin = Position;
			const auto Tag = DecodeInteger(Data, Position);
			if ((Tag >> 3) >= (1u << 29))
				throw std::runtime_error("Invalid protobuf field number.");
			ProtoField Field;
			Field.Number = static_cast<std::uint32_t>(Tag >> 3);
			Field.WireType = static_cast<std::uint8_t>(Tag & 7);
			CheckNumber(Field.Number);
			if (Field.WireType == 0)
				Field.Integer = DecodeInteger(Data, Position);
			else
			{
				const auto Length = Field.WireType == 2   ? DecodeInteger(Data, Position)
									: Field.WireType == 1 ? 8ull
									: Field.WireType == 5 ? 4ull
														  : UINT64_MAX;
				if (Length > Data.size() - Position)
					throw std::runtime_error("Truncated or unsupported protobuf field.");
				const auto End = Position + static_cast<std::size_t>(Length);
				Field.Data.assign(Data.begin() + Position, Data.begin() + End);
				Position = End;
			}
			Field.Raw.assign(Data.begin() + Begin, Data.begin() + Position);
			Result.Members.push_back(std::move(Field));
			if (Result.Members.size() > 1000000)
				throw std::runtime_error("Too many protobuf fields.");
		}
		return Result;
	}

	Bytes ProtoMessage::Serialize() const
	{
		Bytes Result;
		for (const auto &Field : Members)
		{
			if (Field.Raw.size() > MaxSaveBytes - Result.size())
				throw std::runtime_error("Save message exceeds the bounded size limit.");
			Result.insert(Result.end(), Field.Raw.begin(), Field.Raw.end());
		}
		return Result;
	}

	std::vector<ProtoField> ProtoMessage::Select(std::uint32_t Number) const
	{
		std::vector<ProtoField> Result;
		for (const auto &Field : Members)
			if (Field.Number == Number)
				Result.push_back(Field);
		return Result;
	}

	const ProtoField *ProtoMessage::Scalar(std::uint32_t Number, std::uint8_t WireType) const
	{
		const ProtoField *Result = nullptr;
		for (const auto &Field : Members)
		{
			if (Field.Number != Number)
				continue;
			if (Result || Field.WireType != WireType)
				throw std::runtime_error("Duplicate or incorrectly typed save field.");
			Result = &Field;
		}
		return Result;
	}

	std::uint64_t ProtoMessage::Integer(std::uint32_t Number, std::uint64_t Default) const
	{
		const auto Field = Scalar(Number, 0);
		return Field ? Field->Integer : Default;
	}

	Bytes ProtoMessage::Blob(std::uint32_t Number) const
	{
		const auto Field = Scalar(Number, 2);
		return Field ? Field->Data : Bytes{};
	}

	std::string ProtoMessage::String(std::uint32_t Number) const
	{
		const auto Data = Blob(Number);
		return std::string(Data.begin(), Data.end());
	}

	double ProtoMessage::Double(std::uint32_t Number, double Default) const
	{
		const auto Field = Scalar(Number, 1);
		if (!Field)
			return Default;
		std::uint64_t Bits = 0;
		for (unsigned Index = 0; Index < 8; ++Index)
			Bits |= static_cast<std::uint64_t>(Field->Data[Index]) << (Index * 8);
		return std::bit_cast<double>(Bits);
	}

	std::vector<std::uint32_t> ProtoMessage::PackedIntegers(std::uint32_t Number) const
	{
		std::vector<std::uint32_t> Result;
		for (const auto &Field : Members)
		{
			if (Field.Number != Number)
				continue;
			if (Field.WireType == 0)
			{
				if (Field.Integer > UINT32_MAX)
					throw std::runtime_error("Save integer exceeds uint32.");
				Result.push_back(static_cast<std::uint32_t>(Field.Integer));
			}
			else if (Field.WireType == 2)
			{
				std::size_t Position = 0;
				while (Position < Field.Data.size())
				{
					const auto Value = DecodeInteger(Field.Data, Position);
					if (Value > UINT32_MAX)
						throw std::runtime_error("Save integer exceeds uint32.");
					Result.push_back(static_cast<std::uint32_t>(Value));
				}
			}
			else
				throw std::runtime_error("Incorrect packed integer field type.");
		}
		return Result;
	}

	void ProtoMessage::Replace(std::uint32_t Number, const std::vector<ProtoField> &Fields)
	{
		CheckNumber(Number);
		for (const auto &Field : Fields)
			if (Field.Number != Number)
				throw std::invalid_argument("Replacement save field number mismatch.");
		std::vector<ProtoField> Result;
		bool Inserted = false;
		for (const auto &Field : Members)
		{
			if (Field.Number != Number)
				Result.push_back(Field);
			else if (!Inserted)
			{
				Result.insert(Result.end(), Fields.begin(), Fields.end());
				Inserted = true;
			}
		}
		if (!Inserted)
			Result.insert(Result.end(), Fields.begin(), Fields.end());
		Members = std::move(Result);
	}

	void ProtoMessage::Add(ProtoField Field)
	{
		Members.push_back(std::move(Field));
	}

	SaveDocument SaveDocument::Decode(std::span<const std::uint8_t> Compressed)
	{
		if (Compressed.size() < 7 || Compressed.size() > MaxSaveBytes || Compressed[0] != 4 || Compressed[1] != 0x22 ||
			Compressed[2] != 0x4d || Compressed[3] != 0x18)
			throw std::runtime_error("This file is not a supported LZ4 game save.");
		Lz4Reader Reader;
		SaveDocument Result;
		std::size_t Consumed = Compressed.size();
		CheckLz4(LZ4F_getFrameInfo(Reader.Handle, &Result.FrameInfo, Compressed.data(), &Consumed));
		if (Result.FrameInfo.dictID || Result.FrameInfo.contentSize > MaxSaveBytes)
			throw std::runtime_error("Unsupported LZ4 save dictionary or size.");
		std::size_t Position = Consumed;
		std::array<std::uint8_t, 65536> Block{};
		for (;;)
		{
			std::size_t InputSize = Compressed.size() - Position;
			std::size_t OutputSize = Block.size();
			const auto Remaining = LZ4F_decompress(Reader.Handle, Block.data(), &OutputSize,
												   Compressed.data() + Position, &InputSize, nullptr);
			CheckLz4(Remaining);
			Position += InputSize;
			if (OutputSize > MaxSaveBytes - Result.OriginalPayload.size())
				throw std::runtime_error("Decompressed save exceeds the bounded size limit.");
			Result.OriginalPayload.insert(Result.OriginalPayload.end(), Block.begin(), Block.begin() + OutputSize);
			if (Remaining == 0)
				break;
			if (InputSize == 0 && OutputSize == 0)
				throw std::runtime_error("Truncated LZ4 game save.");
		}
		if (Position != Compressed.size())
			throw std::runtime_error("Unexpected data after the LZ4 save frame.");
		Result.Root = ProtoMessage::Parse(Result.OriginalPayload);
		if (Result.Root.String(1) != "AutoSave")
			throw std::runtime_error("The file is not a game AutoSave document.");
		Result.OriginalBytes.assign(Compressed.begin(), Compressed.end());
		return Result;
	}

	Bytes SaveDocument::Encode() const
	{
		const auto Payload = Root.Serialize();
		if (Payload == OriginalPayload)
			return OriginalBytes;
		LZ4F_preferences_t Preferences{};
		Preferences.frameInfo = FrameInfo;
		Preferences.frameInfo.contentSize = FrameInfo.contentSize ? Payload.size() : 0;
		const auto Bound = LZ4F_compressFrameBound(Payload.size(), &Preferences);
		CheckLz4(Bound);
		Bytes Result(Bound);
		const auto Size =
			LZ4F_compressFrame(Result.data(), Result.size(), Payload.data(), Payload.size(), &Preferences);
		CheckLz4(Size);
		Result.resize(Size);
		return Result;
	}

	std::size_t SaveDocument::ModelIndex(const std::string &Name) const
	{
		std::size_t Result = SIZE_MAX;
		const auto &Fields = Root.Fields();
		for (std::size_t Index = 0; Index < Fields.size(); ++Index)
		{
			const auto &Field = Fields[Index];
			if (Field.Number != 2)
				continue;
			if (Field.WireType != 2)
				throw std::runtime_error("Invalid save model field type.");
			const auto Model = ProtoMessage::Parse(Field.Data);
			if (Model.String(1) == Name)
			{
				if (Result != SIZE_MAX)
					throw std::runtime_error("Duplicate named save model.");
				Result = Index;
			}
		}
		if (Result == SIZE_MAX)
			throw std::runtime_error(
				"The save lacks the current data-body model; load and save it in the current game first.");
		return Result;
	}

	Bytes SaveDocument::Model(const std::string &Name) const
	{
		return ProtoMessage::Parse(Root.Fields()[ModelIndex(Name)].Data).Blob(2);
	}

	void SaveDocument::ReplaceModel(const std::string &Name, const Bytes &Payload)
	{
		const auto Index = ModelIndex(Name);
		auto Updated = ProtoMessage::Parse(Root.Fields()[Index].Data);
		Updated.Blob(2);
		Updated.Replace(2, {ProtoField::Blob(2, Payload)});
		ProtoMessage NewRoot;
		for (std::size_t Position = 0; Position < Root.Fields().size(); ++Position)
			NewRoot.Add(Position == Index ? ProtoField::Blob(2, Updated.Serialize()) : Root.Fields()[Position]);
		Root = std::move(NewRoot);
	}
} // namespace Trainer
