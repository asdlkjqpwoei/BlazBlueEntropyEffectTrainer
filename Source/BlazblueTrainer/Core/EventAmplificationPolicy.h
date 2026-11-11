#pragma once

#include <cstdint>

namespace Trainer
{
	// Verified against EDungeonRoomEventType / EDungeonRoomEventRewardType.
	// SanBuff is the game's consciousness-amplification reward, not SanDebuff.
	constexpr bool IsGuaranteedAmplificationReward(
		std::int32_t EventType, std::int32_t RewardType,
		std::int32_t Amount, std::int32_t Probability) noexcept
	{
		return EventType == 0 && RewardType == 11 && Amount > 0 &&
			(Probability == 0 || Probability == 10000);
	}
}
