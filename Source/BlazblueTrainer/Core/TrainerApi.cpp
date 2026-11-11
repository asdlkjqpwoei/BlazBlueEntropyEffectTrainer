#include "TrainerEngine.h"
#include "Text.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>

struct TrainerEngine
{
	explicit TrainerEngine(Trainer::Config Configuration)
		: Implementation(std::move(Configuration))
	{
	}

	Trainer::Engine Implementation;
};

namespace
{
	void CopyError(char* Buffer, std::size_t Size, const std::string& Message)
	{
		Trainer::CopyUtf8(Buffer, Size, Message);
	}
}

extern "C"
{
	TRAINER_API TrainerEngine* __cdecl TrainerCreate(
		const wchar_t* ConfigPath,
		char* ErrorBuffer,
		std::size_t ErrorBufferSize)
	{
		if (ConfigPath == nullptr || ConfigPath[0] == L'\0')
		{
			CopyError(ErrorBuffer, ErrorBufferSize, "A configuration path is required.");
			return nullptr;
		}

		try
		{
			Trainer::Config Configuration = Trainer::LoadConfig(std::filesystem::path(ConfigPath));
			CopyError(ErrorBuffer, ErrorBufferSize, {});
			return new TrainerEngine(std::move(Configuration));
		}
		catch (const YAML::Exception& Exception)
		{
			CopyError(ErrorBuffer, ErrorBufferSize, std::string("YAML configuration error: ") + Exception.what());
		}
		catch (const std::filesystem::filesystem_error& Exception)
		{
			CopyError(ErrorBuffer, ErrorBufferSize, std::string("Configuration file error: ") + Exception.what());
		}
		catch (const std::exception& Exception)
		{
			CopyError(ErrorBuffer, ErrorBufferSize, Exception.what());
		}
		catch (...)
		{
			CopyError(ErrorBuffer, ErrorBufferSize, "Unknown error while loading trainer configuration.");
		}
		return nullptr;
	}

	TRAINER_API void __cdecl TrainerDestroy(TrainerEngine* Engine)
	{
		delete Engine;
	}

	TRAINER_API std::int32_t __cdecl TrainerStart(
		TrainerEngine* Engine,
		char* ErrorBuffer,
		std::size_t ErrorBufferSize)
	{
		if (Engine == nullptr)
		{
			CopyError(ErrorBuffer, ErrorBufferSize, "Trainer engine is not initialized.");
			return 0;
		}

		try
		{
			std::string Error;
			const bool Started = Engine->Implementation.Start(Error);
			CopyError(ErrorBuffer, ErrorBufferSize, Engine->Implementation.Translate(Error));
			return Started ? 1 : 0;
		}
		catch (const std::exception& Exception)
		{
			CopyError(ErrorBuffer, ErrorBufferSize, Engine->Implementation.Translate(Exception.what()));
			return 0;
		}
	}

	TRAINER_API void __cdecl TrainerStop(TrainerEngine* Engine)
	{
		if (Engine != nullptr)
		{
			Engine->Implementation.Stop();
		}
	}

	TRAINER_API std::int32_t __cdecl TrainerSetLanguage(
		TrainerEngine* Engine, TrainerLanguage Language,
		char* ErrorBuffer, std::size_t ErrorBufferSize)
	{
		if (!Engine)
		{
			CopyError(ErrorBuffer, ErrorBufferSize, "Trainer engine is not initialized.");
			return 0;
		}
		try
		{
			Engine->Implementation.SetLanguage(Language);
			CopyError(ErrorBuffer, ErrorBufferSize, {});
			return 1;
		}
		catch (const std::exception& Error)
		{
			CopyError(ErrorBuffer, ErrorBufferSize, Engine->Implementation.Translate(Error.what()));
			return 0;
		}
	}

	TRAINER_API std::int32_t __cdecl TrainerGetSnapshot(
		TrainerEngine* Engine,
		TrainerSnapshot* Snapshot,
		char* ErrorBuffer,
		std::size_t ErrorBufferSize)
	{
		if (Engine == nullptr || Snapshot == nullptr)
		{
			const std::string Message = "A trainer engine and snapshot destination are required.";
			CopyError(ErrorBuffer, ErrorBufferSize, Engine ? Engine->Implementation.Translate(Message) : Message);
			return 0;
		}

		*Snapshot = Engine->Implementation.Snapshot();
		CopyError(ErrorBuffer, ErrorBufferSize, {});
		return 1;
	}

	TRAINER_API std::int32_t __cdecl TrainerApply(
		TrainerEngine* Engine,
		TrainerFeature Feature,
		TrainerOperation Operation,
		std::int64_t Value,
		std::int32_t* Writes,
		char* ErrorBuffer,
		std::size_t ErrorBufferSize)
	{
		if (Engine == nullptr || Writes == nullptr)
		{
			const std::string Message = "A trainer engine and write-count destination are required.";
			CopyError(ErrorBuffer, ErrorBufferSize, Engine ? Engine->Implementation.Translate(Message) : Message);
			return 0;
		}

		try
		{
			std::string Error;
			const bool Applied = Engine->Implementation.Apply(Feature, Operation, Value, *Writes, Error);
			CopyError(ErrorBuffer, ErrorBufferSize, Engine->Implementation.Translate(Error));
			return Applied ? 1 : 0;
		}
		catch (const std::exception& Exception)
		{
			CopyError(ErrorBuffer, ErrorBufferSize, Engine->Implementation.Translate(Exception.what()));
			return 0;
		}
		catch (...)
		{
			CopyError(ErrorBuffer, ErrorBufferSize,
				Engine->Implementation.Translate("Unknown error while applying the requested change."));
			return 0;
		}
	}

	TRAINER_API std::int32_t __cdecl TrainerGrantAllPotentials(
		TrainerEngine* Engine, TrainerPotentialResult* Result,
		char* ErrorBuffer, std::size_t ErrorBufferSize)
	{
		if (Result)
			*Result = {};
		if (!Engine || !Result)
		{
			const std::string Message = "A trainer engine and potential-result destination are required.";
			CopyError(ErrorBuffer, ErrorBufferSize, Engine ? Engine->Implementation.Translate(Message) : Message);
			return 0;
		}
		try
		{
			std::string Error;
			const bool Applied = Engine->Implementation.GrantAllPotentials(*Result, Error);
			CopyError(ErrorBuffer, ErrorBufferSize, Engine->Implementation.Translate(Error));
			return Applied ? 1 : 0;
		}
		catch (const std::exception& Exception)
		{
			CopyError(ErrorBuffer, ErrorBufferSize, Engine->Implementation.Translate(Exception.what()));
			return 0;
		}
	}

	TRAINER_API std::int32_t __cdecl TrainerSetPerfectGuard(
		TrainerEngine* Engine, std::int32_t Enabled, char* ErrorBuffer, std::size_t ErrorBufferSize)
	{
		if (!Engine || (Enabled != 0 && Enabled != 1))
		{
			CopyError(ErrorBuffer, ErrorBufferSize, "A trainer engine and a valid perfect-guard switch are required.");
			return 0;
		}
		try
		{
			std::string Error;
			const bool Applied = Engine->Implementation.SetPerfectGuard(Enabled != 0, Error);
			CopyError(ErrorBuffer, ErrorBufferSize, Engine->Implementation.Translate(Error));
			return Applied ? 1 : 0;
		}
		catch (const std::exception& Failure)
		{
			CopyError(ErrorBuffer, ErrorBufferSize, Engine->Implementation.Translate(Failure.what()));
			return 0;
		}
	}

	TRAINER_API std::int32_t __cdecl TrainerSetEventAmplification(
		TrainerEngine* Engine, std::int32_t Enabled, char* ErrorBuffer, std::size_t ErrorBufferSize)
	{
		if (!Engine || (Enabled != 0 && Enabled != 1))
		{
			const std::string Message = "A trainer engine and a valid event-amplification switch are required.";
			CopyError(ErrorBuffer, ErrorBufferSize, Engine ? Engine->Implementation.Translate(Message) : Message);
			return 0;
		}
		try
		{
			std::string Error;
			const bool Applied = Engine->Implementation.SetEventAmplification(Enabled != 0, Error);
			CopyError(ErrorBuffer, ErrorBufferSize, Engine->Implementation.Translate(Error));
			return Applied ? 1 : 0;
		}
		catch (const std::exception& Failure)
		{
			CopyError(ErrorBuffer, ErrorBufferSize, Engine->Implementation.Translate(Failure.what()));
			return 0;
		}
	}

	TRAINER_API std::int32_t __cdecl TrainerGetText(
		TrainerEngine* Engine,
		const char* Key,
		char* Output,
		std::size_t OutputSize)
	{
		if (Engine == nullptr || Key == nullptr || Output == nullptr || OutputSize == 0)
		{
			return 0;
		}

		try
		{
			const std::string Text = Engine->Implementation.Text(Key);
			if (Text.size() + 1 > OutputSize)
			{
				return 0;
			}
			std::memcpy(Output, Text.c_str(), Text.size() + 1);
			return 1;
		}
		catch (const std::exception&)
		{
			return 0;
		}
	}

	TRAINER_API std::int64_t __cdecl TrainerGetDefault(
		TrainerEngine* Engine,
		TrainerFeature Feature,
		TrainerOperation Operation)
	{
		if (Engine == nullptr)
		{
			return 0;
		}
		return Engine->Implementation.Default(Feature, Operation);
	}
}
