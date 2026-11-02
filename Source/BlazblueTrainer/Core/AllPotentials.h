#pragma once

#include "TrainerApi.h"

#include <Windows.h>

#include <stop_token>
#include <string>
#include <string_view>

namespace Trainer
{
    bool ParsePotentialResult(
        std::string_view Response, TrainerPotentialResult& Result, std::string& Error);
    bool RequestAllPotentials(
        DWORD ProcessId, TrainerPotentialResult& Result, std::string& Error,
        std::stop_token StopToken = {});
}
