#include "EventAmplification.h"
#include "EventAmplificationPolicy.h"
#include "EventAmplificationProtocol.h"
#include "TrainerApi.h"

#include <array>
#include <stdexcept>

namespace
{
	void Require(bool Value, const char* Message)
	{
		if (!Value) throw std::runtime_error(Message);
	}
}

int RunEventAmplificationTests()
{
	using namespace Trainer;
	Require(IsGuaranteedAmplificationReward(0, 11, 1, 0), "Unconditional amplification must qualify.");
	Require(IsGuaranteedAmplificationReward(0, 11, 3, 10000), "100-percent amplification must qualify.");
	for (const auto Probability : {-1, 1, 5000, 9999, 10001})
		Require(!IsGuaranteedAmplificationReward(0, 11, 3, Probability), "A chance reward must not satisfy the guarantee.");
	Require(!IsGuaranteedAmplificationReward(0, 12, 1, 0), "A SanDebuff is not an amplification.");
	Require(!IsGuaranteedAmplificationReward(3, 11, 1, 0), "Elite challenge rewards must not qualify.");
	Require(!IsGuaranteedAmplificationReward(0, 11, -1, 0), "Removing an amplification must not qualify.");
	Require(!IsGuaranteedAmplificationReward(0, 11, 0, 0), "An empty reward must not qualify.");
	for (const auto State : {TrainerStateChecking, TrainerStateScanning, TrainerStateReady})
		Require(KeepEventAmplificationForStatus(State, true), "Scene transitions must preserve the switch.");
	for (const auto State : {TrainerStateStopped, TrainerStateWaiting, TrainerStateWrongCopy,
		TrainerStateUnsupportedBuild, TrainerStateAccessDenied, TrainerStateError})
		Require(!KeepEventAmplificationForStatus(State, true), "Lost or unsupported sessions must disable the switch.");
	Require(!KeepEventAmplificationForStatus(TrainerStateReady, false), "A missing game must disable the switch.");
	Require(EventAmplificationHeartbeatAlive(5999, 1000) && !EventAmplificationHeartbeatAlive(6000, 1000) &&
		!EventAmplificationHeartbeatAlive(999, 1000), "Heartbeat expiry must fail closed.");
	EventAmplificationCommand Command;
	Command.RequesterProcessId = 7;
	Command.TargetProcessId = 8;
	Command.Nonce = 9;
	Require(ValidEventAmplificationCommand(Command, 7, 8, 9), "The valid command was rejected.");
	Require(!ValidEventAmplificationCommand(Command, 7, 8, 10), "A foreign nonce was accepted.");
	++Command.Version;
	Require(!ValidEventAmplificationCommand(Command, 7, 8, 9), "An incompatible command was accepted.");
	EventAmplification Feature;
	std::string Error;
	std::stop_source Cancellation;
	Cancellation.request_stop();
	Require(!Feature.Enable(7, Error, Cancellation.get_token()) && !Feature.Enabled(7), "A cancelled feature was enabled.");
	Require(!Feature.Enable(0, Error), "An absent target was accepted.");
	Feature.Disable();
	Feature.Disable();
	std::array<char, 512> Buffer{};
	Require(!TrainerSetEventAmplification(nullptr, 1, Buffer.data(), Buffer.size()), "A null engine was accepted.");
	return 0;
}
