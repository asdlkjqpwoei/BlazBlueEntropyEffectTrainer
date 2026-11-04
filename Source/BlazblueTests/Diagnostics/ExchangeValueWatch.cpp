#include "Config.h"
#include "Paths.h"
#include "RemoteProcess.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace
{
	constexpr std::size_t WatchPageSize = 4096;
	constexpr std::size_t MaximumHits = 200000;

	struct Hit
	{
		std::uintptr_t Address{};
		std::uint32_t LastValue{};
	};

	struct WatchPage
	{
		std::uintptr_t Base{};
		std::vector<Hit> Hits;
		bool Readable{ true };
	};

	bool ParseUnsigned(std::string_view Text, std::uint32_t& Value)
	{
		const auto [end, Error] = std::from_chars(
			Text.data(),
			Text.data() + Text.size(),
			Value);
		return Error == std::errc{} && end == Text.data() + Text.size();
	}

	void LogChangedValue(
		std::uintptr_t Address,
		std::uint32_t Previous,
		std::uint32_t Current)
	{
		std::cout << "change address=0x" << std::hex << Address << std::dec
			<< " previous=" << Previous
			<< " current=" << Current << '\n' << std::flush;
	}
}

int RunExchangeWatch(int ArgumentCount, char** Arguments)
{
	if (ArgumentCount != 5 ||
		std::string_view(Arguments[1]) != "--target-value" ||
		std::string_view(Arguments[3]) != "--watch-seconds")
	{
		std::cerr << "Usage: exchange_value_watch --target-value <count> "
			<< "--watch-seconds <seconds>\n";
		return 1;
	}

	std::uint32_t TargetValue = 0;
	std::uint32_t WatchSeconds = 0;
	if (!ParseUnsigned(Arguments[2], TargetValue) ||
		!ParseUnsigned(Arguments[4], WatchSeconds) ||
		WatchSeconds < 600)
	{
		std::cerr << "Provide an unsigned target count and watch duration of at least 600 seconds.\n";
		return 1;
	}

	try
	{
		const Trainer::Config Configuration = Trainer::LoadConfig(
			Trainer::DefaultConfigPath());
		const Trainer::AttachResult Attach =
			Trainer::RemoteProcess::FindCompatibleGameProcess(Configuration);
		if (Attach.Status != Trainer::AttachStatus::Attached || !Attach.Process)
		{
			std::cerr << "Could not attach to a compatible game process: "
				<< Attach.Message << '\n';
			return 2;
		}

		std::cout << "Read-only value watch: pid=" << Attach.Process->ProcessId()
			<< " baseline=" << TargetValue << " duration_seconds=" << WatchSeconds
			<< "\nScanning writable private memory before monitoring.\n" << std::flush;

		std::unordered_map<std::uintptr_t, std::size_t> PageIndexes;
		std::vector<WatchPage> Pages;
		std::uintptr_t MemoryCursor = 0;
		std::size_t ScanPass = 0;
		std::size_t HitCount = 0;
		bool HitLimitReached = false;

		do
		{
			bool Truncated = false;
			std::string ScanError;
			const std::vector<Trainer::MemoryRegion> Regions =
				Attach.Process->WritablePrivateRegions(
					Configuration.Monitor.MaxScanBytes,
					MemoryCursor,
					Truncated,
					ScanError);
			if (!ScanError.empty())
			{
				std::cerr << "Memory enumeration failed: " << ScanError << '\n';
				return 2;
			}

			std::vector<std::byte> Buffer(Configuration.Monitor.ScanChunkBytes);
			for (const Trainer::MemoryRegion& Region : Regions)
			{
				std::size_t Consumed = 0;
				while (Consumed < Region.Size)
				{
					const std::size_t Request = std::min(
						Buffer.size(),
						Region.Size - Consumed);
					const std::uintptr_t ChunkAddress = Region.Base + Consumed;
					if (Attach.Process->Read(
							ChunkAddress,
							Buffer.data(),
							Request))
					{
						const std::size_t InitialOffset =
							static_cast<std::size_t>((4 - (ChunkAddress & 3)) & 3);
						for (std::size_t Offset = InitialOffset;
							 Offset + sizeof(std::uint32_t) <= Request;
							 Offset += sizeof(std::uint32_t))
						{
							std::uint32_t Value = 0;
							std::memcpy(
								&Value,
								Buffer.data() + Offset,
								sizeof(Value));
							if (Value != TargetValue)
							{
								continue;
							}

							const std::uintptr_t Address = ChunkAddress + Offset;
							const std::uintptr_t PageBase =
								Address & ~static_cast<std::uintptr_t>(WatchPageSize - 1);
							auto [Page, inserted] = PageIndexes.try_emplace(
								PageBase,
								Pages.size());
							if (inserted)
							{
								Pages.push_back({ PageBase, {}, true });
							}
							Pages[Page->second].Hits.push_back({ Address, Value });
							++HitCount;
							if (HitCount >= MaximumHits)
							{
								HitLimitReached = true;
								break;
							}
						}
					}
					Consumed += Request;
					if (HitLimitReached)
					{
						break;
					}
				}
				if (HitLimitReached)
				{
					break;
				}
			}

			++ScanPass;
			std::cout << "baseline_pass=" << ScanPass
				<< " cursor=0x" << std::hex << MemoryCursor << std::dec
				<< " regions=" << Regions.size()
				<< " truncated=" << Truncated
				<< " matches_so_far=" << HitCount << '\n' << std::flush;
			if (HitLimitReached)
			{
				break;
			}
		} while (MemoryCursor != 0);

		const bool BaselineComplete = MemoryCursor == 0 && !HitLimitReached;
		std::cout << "baseline_complete=" << BaselineComplete
			<< " matches=" << HitCount
			<< " pages=" << Pages.size()
			<< " hit_limit_reached=" << HitLimitReached << '\n';
		if (!BaselineComplete || HitCount == 0)
		{
			std::cout << "Monitoring was not started because the baseline is incomplete or empty.\n"
				<< std::flush;
			return 3;
		}

		const std::size_t SampleCount = std::min<std::size_t>(HitCount, 32);
		std::size_t Printed = 0;
		for (const WatchPage& Page : Pages)
		{
			for (const Hit& Hit : Page.Hits)
			{
				if (Printed >= SampleCount)
				{
					break;
				}
				std::cout << "baseline_hit address=0x" << std::hex << Hit.Address
					<< std::dec << " value=" << Hit.LastValue << '\n';
				++Printed;
			}
			if (Printed >= SampleCount)
			{
				break;
			}
		}
		std::cout << "Monitoring is active; change the exchange count only after this baseline.\n"
			<< std::flush;

		const auto WatchStart = std::chrono::steady_clock::now();
		const auto Deadline = WatchStart +
			std::chrono::seconds(WatchSeconds);
		const auto PollInterval = Pages.size() > 10000
			? std::chrono::milliseconds(1000)
			: Pages.size() > 1000
				? std::chrono::milliseconds(500)
				: std::chrono::milliseconds(250);
		std::size_t Changes = 0;
		while (std::chrono::steady_clock::now() < Deadline)
		{
			if (!Attach.Process->IsAlive())
			{
				std::cout << "process_exited=1\n" << std::flush;
				break;
			}

			std::array<std::byte, WatchPageSize> Buffer{};
			for (WatchPage& Page : Pages)
			{
				if (!Page.Readable)
				{
					continue;
				}
				if (!Attach.Process->Read(Page.Base, Buffer.data(), Buffer.size()))
				{
					Page.Readable = false;
					std::cout << "page_unreadable=0x" << std::hex << Page.Base
						<< std::dec << '\n' << std::flush;
					continue;
				}

				for (Hit& Hit : Page.Hits)
				{
					const std::size_t Offset =
						static_cast<std::size_t>(Hit.Address - Page.Base);
					if (Offset + sizeof(std::uint32_t) > Buffer.size())
					{
						continue;
					}
					std::uint32_t Current = 0;
					std::memcpy(&Current, Buffer.data() + Offset, sizeof(Current));
					if (Current != Hit.LastValue)
					{
						LogChangedValue(Hit.Address, Hit.LastValue, Current);
						Hit.LastValue = Current;
						++Changes;
					}
				}
			}

			std::this_thread::sleep_for(PollInterval);
		}

		const auto ElapsedSeconds = std::chrono::duration_cast<std::chrono::seconds>(
			std::chrono::steady_clock::now() - WatchStart).count();
		std::cout << "watch_complete=1 changes=" << Changes
			<< " elapsed_seconds=" << ElapsedSeconds << '\n' << std::flush;
		return 0;
	}
	catch (const std::exception& Exception)
	{
		std::cerr << "Exchange value watch failed: " << Exception.what() << '\n';
		return 2;
	}
}
