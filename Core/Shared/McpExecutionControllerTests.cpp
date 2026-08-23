#include "pch.h"
#include "Shared/McpExecutionController.h"

namespace McpExecutionControllerTests {

static uint32_t _failures = 0;
#define CHECK(condition) do { if(!(condition)) { _failures++; } } while(0)

static void TestTransitions()
{
	McpExecutionController controller;
	CHECK(controller.GetSnapshot().State == McpExecutionState::Detached);
	controller.Attach(true);
	CHECK(controller.GetSnapshot().State == McpExecutionState::ClockPaused);
	CHECK(controller.RequestRun());
	CHECK(controller.GetSnapshot().State == McpExecutionState::Running);
	CHECK(controller.RequestPause());
	CHECK(controller.GetSnapshot().State == McpExecutionState::PauseRequested);
	controller.NotifyDebuggerStopped(false);
	CHECK(controller.GetSnapshot().State == McpExecutionState::DebuggerPaused);
	CHECK(controller.RequestStep());
	CHECK(controller.GetSnapshot().State == McpExecutionState::Stepping);
	controller.NotifyDebuggerStopped(false);
	CHECK(controller.GetSnapshot().State == McpExecutionState::DebuggerPaused);
}

static void TestBreakpointLifecycle()
{
	McpExecutionController controller;
	controller.Attach(true);
	controller.RequestRun();
	controller.NotifyDebuggerStopped(true);
	CHECK(controller.GetSnapshot().State == McpExecutionState::BreakpointStopped);
	CHECK(controller.RequestRun());
	CHECK(controller.GetSnapshot().State == McpExecutionState::Running);
}

static void TestInvalidTransitions()
{
	McpExecutionController controller;
	CHECK(!controller.RequestRun());
	controller.Attach(true);
	CHECK(!controller.RequestPause());
	controller.RequestRun();
	CHECK(!controller.RequestRun());
	CHECK(!controller.RequestStep());
}

static void TestPauseKindsAreExclusive()
{
	McpExecutionController controller;
	controller.Attach(true);
	CHECK(controller.GetSnapshot().State == McpExecutionState::ClockPaused);
	CHECK(controller.IsClockGated());
	controller.RequestRun();
	controller.NotifyDebuggerStopped(false);
	CHECK(controller.GetSnapshot().State == McpExecutionState::DebuggerPaused);
	CHECK(!controller.IsClockGated());
	controller.NotifyDebuggerResumed();
	CHECK(controller.GetSnapshot().State == McpExecutionState::Running);
}

static void TestDetachedDoesNotControlMesenClock()
{
	McpExecutionController controller;
	CHECK(!controller.IsAttached());
	CHECK(!controller.IsClockGated());
}

static void TestQueueOwnershipAndOrder()
{
	McpExecutionController controller;
	vector<int> order;
	vector<McpExecutionPoint> points;
	controller.Enqueue([&](McpExecutionPoint point) { order.push_back(1); points.push_back(point); });
	controller.Enqueue([&](McpExecutionPoint point) { order.push_back(2); points.push_back(point); });
	CHECK(order.empty());
	CHECK(controller.Pump(McpExecutionPoint::DebuggerStop) == 2);
	CHECK(order == vector<int>({ 1, 2 }));
	CHECK(points.size() == 2 && points[0] == McpExecutionPoint::DebuggerStop && points[1] == McpExecutionPoint::DebuggerStop);
	CHECK(controller.Pump(McpExecutionPoint::MainLoop) == 0);
}

uint32_t RunAll()
{
	_failures = 0;
	TestTransitions();
	TestBreakpointLifecycle();
	TestInvalidTransitions();
	TestPauseKindsAreExclusive();
	TestDetachedDoesNotControlMesenClock();
	TestQueueOwnershipAndOrder();
	return _failures;
}

}
