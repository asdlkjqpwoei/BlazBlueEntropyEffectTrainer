#include "GameData.h"
#include "Paths.h"

#include <Windows.h>
#include <lz4.h>
#include <shlobj.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <regex>
#include <stdexcept>
#include <string_view>

namespace Trainer
{
	namespace
	{
		constexpr std::size_t MaximumAssetSize = 64 * 1024 * 1024;

		class Cursor
		{
		  public:
			explicit Cursor(std::span<const std::uint8_t> Data, bool LittleEndian = false)
				: Data(Data), Little(LittleEndian)
			{
			}
			void Skip(std::uint64_t Size)
			{
				if (Size > Data.size() - Position)
					throw std::runtime_error("Truncated game asset.");
				Position += static_cast<std::size_t>(Size);
			}
			void Align(std::size_t Alignment)
			{
				Skip((Alignment - Position % Alignment) % Alignment);
			}
			std::uint8_t U8()
			{
				const auto Value = Slice(1);
				return Value[0];
			}
			std::uint16_t U16()
			{
				return static_cast<std::uint16_t>(Unsigned(2));
			}
			std::uint32_t U32()
			{
				return static_cast<std::uint32_t>(Unsigned(4));
			}
			std::uint64_t U64()
			{
				return Unsigned(8);
			}
			std::span<const std::uint8_t> Slice(std::uint64_t Size)
			{
				const auto Begin = Position;
				Skip(Size);
				return Data.subspan(Begin, static_cast<std::size_t>(Size));
			}
			std::string NullString()
			{
				const auto Begin = Position;
				while (Position < Data.size() && Data[Position])
					++Position;
				if (Position == Data.size() || Position - Begin > 4096)
					throw std::runtime_error("Invalid game asset string.");
				std::string Result(Data.begin() + Begin, Data.begin() + Position);
				++Position;
				return Result;
			}
			std::string SizedString()
			{
				const auto Value = Slice(U32());
				std::string Result(Value.begin(), Value.end());
				Align(4);
				return Result;
			}
			void SetLittle(bool Value)
			{
				Little = Value;
			}
			std::size_t Offset() const
			{
				return Position;
			}

		  private:
			std::uint64_t Unsigned(unsigned Size)
			{
				const auto Value = Slice(Size);
				std::uint64_t Result = 0;
				for (unsigned Index = 0; Index < Size; ++Index)
					Result |= static_cast<std::uint64_t>(Value[Index]) << (8 * (Little ? Index : Size - Index - 1));
				return Result;
			}
			std::span<const std::uint8_t> Data;
			std::size_t Position{};
			bool Little{};
		};

		Bytes Expand(std::span<const std::uint8_t> Compressed, std::size_t Size, unsigned Flags)
		{
			if (Size > MaximumAssetSize)
				throw std::runtime_error("Game asset exceeds the bounded size limit.");
			if ((Flags & 63) == 0)
			{
				if (Compressed.size() != Size)
					throw std::runtime_error("Uncompressed game asset length mismatch.");
				return Bytes(Compressed.begin(), Compressed.end());
			}
			if ((Flags & 63) != 2 && (Flags & 63) != 3)
				throw std::runtime_error("Unsupported game asset compression.");
			Bytes Result(Size);
			const auto Decoded = LZ4_decompress_safe(reinterpret_cast<const char *>(Compressed.data()),
													 reinterpret_cast<char *>(Result.data()),
													 static_cast<int>(Compressed.size()), static_cast<int>(Size));
			if (Decoded < 0 || static_cast<std::size_t>(Decoded) != Size)
				throw std::runtime_error("Invalid compressed game asset.");
			return Result;
		}

		std::optional<Bytes> ReadSerializedText(std::span<const std::uint8_t> Data)
		{
			Cursor Header(Data);
			Header.U32();
			Header.U32();
			const auto Version = Header.U32();
			std::uint64_t DataOffset = Header.U32();
			const bool Little = Header.U8() == 0;
			Header.Skip(3);
			if (Version < 17 || Version > 22)
				throw std::runtime_error("Unsupported serialized game asset version.");
			if (Version >= 22)
			{
				Header.U32();
				const auto Size = Header.U64();
				DataOffset = Header.U64();
				Header.Skip(8);
				if (Size > Data.size())
					throw std::runtime_error("Truncated serialized game asset.");
			}
			Header.SetLittle(Little);
			Header.NullString();
			Header.U32();
			const bool TypeTree = Header.U8() != 0;
			const auto TypeCount = Header.U32();
			if (TypeCount > 10000)
				throw std::runtime_error("Invalid game asset type count.");
			std::vector<std::uint32_t> Types;
			for (std::uint32_t Index = 0; Index < TypeCount; ++Index)
			{
				const auto Class = Header.U32();
				Header.U8();
				Header.U16();
				if (Class == 114)
					Header.Skip(16);
				Header.Skip(16);
				if (TypeTree)
				{
					const auto Nodes = Header.U32();
					const auto Strings = Header.U32();
					Header.Skip(static_cast<std::uint64_t>(Nodes) * (Version >= 19 ? 32 : 24) + Strings);
				}
				if (Version >= 21)
					Header.Skip(static_cast<std::uint64_t>(Header.U32()) * 4);
				Types.push_back(Class);
			}
			const auto Count = Header.U32();
			if (Count > 200000)
				throw std::runtime_error("Invalid game asset object count.");
			std::optional<Bytes> Result;
			for (std::uint32_t Index = 0; Index < Count; ++Index)
			{
				Header.Align(4);
				Header.Skip(8);
				const auto RelativeOffset = Version >= 22 ? Header.U64() : Header.U32();
				const auto Length = Header.U32();
				const auto Type = Header.U32();
				if (Type >= Types.size() || DataOffset > Data.size() || RelativeOffset > Data.size() - DataOffset ||
					Length > Data.size() - DataOffset - RelativeOffset)
					throw std::runtime_error("Invalid game asset object range.");
				if (Types[Type] != 49)
					continue;
				Cursor Object(Data.subspan(static_cast<std::size_t>(DataOffset + RelativeOffset), Length), Little);
				Object.SizedString();
				const auto Payload = Object.Slice(Object.U32());
				if (Result)
					throw std::runtime_error("The requested bundle contains multiple text assets.");
				Result = Bytes(Payload.begin(), Payload.end());
			}
			return Result;
		}

		Bytes UnpackText(std::span<const std::uint8_t> Bundle)
		{
			Cursor Header(Bundle);
			if (Header.NullString() != "UnityFS")
				throw std::runtime_error("The current game data is not a UnityFS bundle.");
			const auto Version = Header.U32();
			Header.NullString();
			Header.NullString();
			const auto TotalSize = Header.U64();
			const auto CompressedSize = Header.U32();
			const auto RawSize = Header.U32();
			const auto Flags = Header.U32();
			if (TotalSize != Bundle.size())
				throw std::runtime_error("Game bundle size mismatch.");
			if (Version >= 7)
				Header.Align(16);
			if (CompressedSize > Bundle.size())
				throw std::runtime_error("Invalid bundle block information.");
			const auto InformationOffset = Flags & 128 ? TotalSize - CompressedSize : Header.Offset();
			if (InformationOffset > Bundle.size() - CompressedSize)
				throw std::runtime_error("Invalid bundle block-information offset.");
			const auto Information =
				Expand(Bundle.subspan(static_cast<std::size_t>(InformationOffset), CompressedSize), RawSize, Flags);
			Cursor Info(Information);
			Info.Skip(16);
			struct Block
			{
				std::uint32_t Raw;
				std::uint32_t Compressed;
				std::uint16_t Flags;
			};
			struct Node
			{
				std::uint64_t Offset;
				std::uint64_t Size;
				std::string Name;
			};
			std::vector<Block> Blocks;
			const auto BlockCount = Info.U32();
			if (BlockCount > 65536)
				throw std::runtime_error("Too many game asset blocks.");
			for (std::uint32_t Index = 0; Index < BlockCount; ++Index)
				Blocks.push_back({Info.U32(), Info.U32(), Info.U16()});
			const auto NodeCount = Info.U32();
			if (NodeCount > 10000)
				throw std::runtime_error("Too many game asset files.");
			std::vector<Node> Nodes;
			for (std::uint32_t Index = 0; Index < NodeCount; ++Index)
			{
				const auto Offset = Info.U64();
				const auto Size = Info.U64();
				Info.U32();
				Nodes.push_back({Offset, Size, Info.NullString()});
			}
			if (!(Flags & 128))
				Header.Skip(CompressedSize);
			if (Flags & 512)
				Header.Align(16);
			Bytes Expanded;
			for (const auto &Block : Blocks)
			{
				if (Block.Raw > MaximumAssetSize - Expanded.size())
					throw std::runtime_error("Expanded bundle exceeds the bounded size limit.");
				const auto Data = Expand(Header.Slice(Block.Compressed), Block.Raw, Block.Flags);
				Expanded.insert(Expanded.end(), Data.begin(), Data.end());
			}
			for (const auto &Node : Nodes)
			{
				if (Node.Offset > Expanded.size() || Node.Size > Expanded.size() - Node.Offset)
					throw std::runtime_error("Invalid expanded bundle file range.");
				if (Node.Name.ends_with(".resS") || Node.Name.ends_with(".resource"))
					continue;
				const auto Text = ReadSerializedText(std::span<const std::uint8_t>(Expanded).subspan(
					static_cast<std::size_t>(Node.Offset), static_cast<std::size_t>(Node.Size)));
				if (Text)
					return *Text;
			}
			throw std::runtime_error("The requested game text asset was not found.");
		}

		std::optional<std::filesystem::path> SteamRoot()
		{
			std::array<wchar_t, 32768> Value{};
			DWORD Size = static_cast<DWORD>(Value.size() * sizeof(wchar_t));
			const auto Status = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ,
											 nullptr, Value.data(), &Size);
			if (Status == ERROR_FILE_NOT_FOUND)
				return std::nullopt;
			if (Status != ERROR_SUCCESS)
				throw std::runtime_error("Cannot read the configured Steam installation.");
			return std::filesystem::path(Value.data());
		}
	} // namespace

	Bytes ReadBinaryFile(const std::filesystem::path &Path, std::size_t Maximum)
	{
		std::ifstream Input(Path, std::ios::binary | std::ios::ate);
		if (!Input)
			throw std::runtime_error("Cannot open the requested file.");
		const auto Length = Input.tellg();
		if (Length < 0 || static_cast<std::uint64_t>(Length) > Maximum)
			throw std::runtime_error("The requested file exceeds its bounded size limit.");
		Bytes Result(static_cast<std::size_t>(Length));
		Input.seekg(0);
		if (!Result.empty() && !Input.read(reinterpret_cast<char *>(Result.data()), Length))
			throw std::runtime_error("Cannot read the complete file.");
		return Result;
	}

	std::optional<std::filesystem::path> FindInstalledGame()
	{
		const auto Root = SteamRoot();
		if (!Root)
			return std::nullopt;
		std::vector<std::filesystem::path> Libraries{*Root};
		const auto LibraryFile = *Root / L"steamapps" / L"libraryfolders.vdf";
		if (std::filesystem::exists(LibraryFile))
		{
			const auto Data = ReadBinaryFile(LibraryFile, 4 * 1024 * 1024);
			const std::string Text(Data.begin(), Data.end());
			const std::regex Pattern(R"vdf("path"\s*"((?:\\.|[^"\\])*)")vdf");
			for (std::sregex_iterator Found(Text.begin(), Text.end(), Pattern), End; Found != End; ++Found)
			{
				auto Value = (*Found)[1].str();
				for (std::size_t Position = 0; (Position = Value.find("\\\\", Position)) != std::string::npos;)
					Value.replace(Position, 2, "\\");
				Libraries.push_back(PathFromUtf8(Value));
			}
		}
		for (const auto &Library : Libraries)
		{
			const auto Manifest = Library / L"steamapps" / L"appmanifest_2273430.acf";
			if (!std::filesystem::exists(Manifest))
				continue;
			const auto Data = ReadBinaryFile(Manifest, 1024 * 1024);
			const std::string Text(Data.begin(), Data.end());
			const std::regex Pattern(R"vdf("installdir"\s*"([^"]+)")vdf");
			std::smatch Match;
			if (!std::regex_search(Text, Match, Pattern))
				throw std::runtime_error("The game installation manifest has no installation directory.");
			const auto Name = PathFromUtf8(Match[1].str());
			if (Name != Name.filename() || Name == L".." || Name == L".")
				throw std::runtime_error("Invalid game installation directory.");
			const auto Directory = Library / L"steamapps" / L"common" / Name;
			if (std::filesystem::is_regular_file(Directory / L"BlazblueEntropyEffect.exe"))
				return Directory;
		}
		return std::nullopt;
	}

	std::vector<std::filesystem::path> FindGameSaves()
	{
		wchar_t *Folder = nullptr;
		const HRESULT Status = SHGetKnownFolderPath(FOLDERID_LocalAppDataLow, 0, nullptr, &Folder);
		if (FAILED(Status))
			throw std::runtime_error("Cannot locate the user's game-save folder.");
		const auto Root = std::filesystem::path(Folder) / L"91Act" / L"BlazBlueEntropyEffect" / L"Steam";
		CoTaskMemFree(Folder);
		std::vector<std::filesystem::path> Result;
		if (!std::filesystem::exists(Root))
			return Result;
		for (const auto &Account : std::filesystem::directory_iterator(Root))
		{
			const auto Save = Account.path() / L"Save";
			if (!Account.is_directory() || !std::filesystem::is_directory(Save))
				continue;
			for (const auto &File : std::filesystem::directory_iterator(Save))
			{
				const auto Name = File.path().filename().wstring();
				if (File.is_regular_file() && !Name.empty() &&
					std::all_of(Name.begin(), Name.end(), [](wchar_t Value) { return Value >= L'0' && Value <= L'9'; }))
					Result.push_back(File.path());
			}
		}
		std::sort(Result.begin(), Result.end(), [](const auto &Left, const auto &Right) {
			return std::filesystem::last_write_time(Left) > std::filesystem::last_write_time(Right);
		});
		return Result;
	}

	GameAssets::GameAssets(const std::filesystem::path &GameDirectory) : Root(GameDirectory)
	{
		const auto Directory = Root / L"BlazblueEntropyEffect_Data" / L"StreamingAssets" / L"ab";
		const auto Raw = ReadBinaryFile(Directory / L"merge.json", 16 * 1024 * 1024);
		const auto Index = YAML::Load(std::string(Raw.begin(), Raw.end()));
		if (!Index.IsSequence() || Index.size() > 200000)
			throw std::runtime_error("Unsupported game resource index.");
		for (const auto &Row : Index)
		{
			const auto Resource = Row["r"].as<std::string>();
			if (!Resource.starts_with("data/xlsx/") && !Resource.starts_with("data/localization/"))
				continue;
			const auto File = PathFromUtf8(Row["m"].as<std::string>());
			if (File != File.filename() || File == L".." || File == L".")
				throw std::runtime_error("Invalid packed game resource location.");
			if (!Locations.emplace(Resource, Location{Directory / File, Row["s"].as<std::uint64_t>()}).second)
				throw std::runtime_error("Duplicate game resource index entry.");
		}
	}

	Bytes GameAssets::TextAsset(const std::string &Resource) const
	{
		const auto Found = Locations.find(Resource);
		if (Found == Locations.end())
			throw std::runtime_error("A required current-game catalog table was not found.");
		std::ifstream Input(Found->second.File, std::ios::binary | std::ios::ate);
		if (!Input)
			throw std::runtime_error("Cannot open the current game's packed data.");
		const auto Length = Input.tellg();
		if (Length < 0 || Found->second.Offset > static_cast<std::uint64_t>(Length))
			throw std::runtime_error("Invalid packed game resource offset.");
		Input.seekg(static_cast<std::streamoff>(Found->second.Offset));
		std::array<std::uint8_t, 128> Header{};
		if (!Input.read(reinterpret_cast<char *>(Header.data()), Header.size()))
			throw std::runtime_error("Cannot read the game bundle header.");
		Cursor Reader(Header);
		if (Reader.NullString() != "UnityFS")
			throw std::runtime_error("Unsupported packed game resource.");
		Reader.U32();
		Reader.NullString();
		Reader.NullString();
		const auto Size = Reader.U64();
		if (Size > MaximumAssetSize || Size > static_cast<std::uint64_t>(Length) - Found->second.Offset)
			throw std::runtime_error("Invalid game bundle size.");
		Bytes Bundle(static_cast<std::size_t>(Size));
		Input.seekg(static_cast<std::streamoff>(Found->second.Offset));
		if (!Input.read(reinterpret_cast<char *>(Bundle.data()), static_cast<std::streamsize>(Bundle.size())))
			throw std::runtime_error("Cannot read the complete game bundle.");
		return UnpackText(Bundle);
	}
} // namespace Trainer
