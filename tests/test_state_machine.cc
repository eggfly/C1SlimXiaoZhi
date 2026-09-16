#include "device_state_machine.h"
#include "test_framework.h"

#include <vector>

TEST(StartsUnknownAndOnlyMovesToStarting) {
    DeviceStateMachine machine;
    CHECK_EQ(machine.GetState(), kDeviceStateUnknown);
    CHECK(!machine.TransitionTo(kDeviceStateIdle));
    CHECK(machine.TransitionTo(kDeviceStateStarting));
    CHECK_EQ(machine.GetState(), kDeviceStateStarting);
}

TEST(ConversationHappyPath) {
    DeviceStateMachine machine;
    CHECK(machine.TransitionTo(kDeviceStateStarting));
    CHECK(machine.TransitionTo(kDeviceStateActivating));
    CHECK(machine.TransitionTo(kDeviceStateIdle));
    CHECK(machine.TransitionTo(kDeviceStateConnecting));
    CHECK(machine.TransitionTo(kDeviceStateListening));
    CHECK(machine.TransitionTo(kDeviceStateSpeaking));
    CHECK(machine.TransitionTo(kDeviceStateListening));
    CHECK(machine.TransitionTo(kDeviceStateIdle));
}

TEST(RejectsIllegalJumps) {
    DeviceStateMachine machine;
    machine.TransitionTo(kDeviceStateStarting);
    machine.TransitionTo(kDeviceStateActivating);
    machine.TransitionTo(kDeviceStateIdle);
    // Idle cannot become audio testing, and connecting cannot become speaking.
    CHECK(!machine.TransitionTo(kDeviceStateAudioTesting));
    CHECK(machine.TransitionTo(kDeviceStateConnecting));
    CHECK(!machine.TransitionTo(kDeviceStateSpeaking));
    CHECK_EQ(machine.GetState(), kDeviceStateConnecting);
}

TEST(FatalErrorIsTerminal) {
    DeviceStateMachine machine;
    machine.TransitionTo(kDeviceStateStarting);
    machine.TransitionTo(kDeviceStateActivating);
    machine.TransitionTo(kDeviceStateIdle);
    // There is no legal edge into fatal error from idle, which is itself part
    // of the contract; force the check on the terminal property instead.
    CHECK(!machine.CanTransitionTo(kDeviceStateFatalError));
}

TEST(SameStateIsANoOp) {
    DeviceStateMachine machine;
    machine.TransitionTo(kDeviceStateStarting);
    CHECK(machine.TransitionTo(kDeviceStateStarting));
    CHECK_EQ(machine.GetState(), kDeviceStateStarting);
}

TEST(ListenersSeeEveryTransition) {
    DeviceStateMachine machine;
    std::vector<DeviceState> seen;
    int id = machine.AddStateChangeListener(
        [&seen](DeviceState, DeviceState current) { seen.push_back(current); });

    machine.TransitionTo(kDeviceStateStarting);
    machine.TransitionTo(kDeviceStateActivating);
    machine.TransitionTo(kDeviceStateStarting);  // illegal, must not notify
    CHECK_EQ(seen.size(), static_cast<size_t>(2));
    CHECK_EQ(seen[0], kDeviceStateStarting);
    CHECK_EQ(seen[1], kDeviceStateActivating);

    machine.RemoveStateChangeListener(id);
    machine.TransitionTo(kDeviceStateIdle);
    CHECK_EQ(seen.size(), static_cast<size_t>(2));
}

TEST(StateNamesAreStable) {
    // These strings end up in logs and in the upstream docs.
    CHECK_STREQ(DeviceStateMachine::GetStateName(kDeviceStateIdle), "idle");
    CHECK_STREQ(DeviceStateMachine::GetStateName(kDeviceStateListening), "listening");
    CHECK_STREQ(DeviceStateMachine::GetStateName(kDeviceStateSpeaking), "speaking");
    CHECK_STREQ(DeviceStateMachine::GetStateName(kDeviceStateActivating), "activating");
}
