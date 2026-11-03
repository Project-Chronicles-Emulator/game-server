// Socket-free tests of the read resume gate (see read_resume_gate.hpp).
#include "net/read_resume_gate.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace chronicles {
namespace {

using Gate = ReadResumeGate;
using Resume = Gate::Resume;
using Park = Gate::Park;

void enqueue(Gate &gate, int frames) {
  for (int index = 0; index < frames; ++index) {
    gate.on_enqueued();
  }
}

TEST(ReadResumeGate, BarrierResumesExactlyWhenTheFramesQueuedAtParkTimeAreWritten) {
  Gate gate;
  enqueue(gate, 50); // notifications of other sessions, already queued
  enqueue(gate, 3);  // this request's response and two notifications
  gate.park_barrier();

  for (int written = 1; written <= 52; ++written) {
    gate.on_written();
    EXPECT_EQ(gate.try_resume_by_barrier(0, 0), Resume::none) << written;
  }
  gate.on_written(); // the 53rd frame: the last one that existed at park time
  EXPECT_EQ(gate.try_resume_by_barrier(0, 0), Resume::by_barrier);
}

TEST(ReadResumeGate, FramesEnqueuedAfterTheParkDoNotExtendTheBarrier) {
  Gate gate;
  enqueue(gate, 2);
  gate.park_barrier();
  // One new notification arrives per completed write, so the queue never drains.
  gate.on_enqueued();
  gate.on_written();
  EXPECT_EQ(gate.try_resume_by_barrier(1, 100), Resume::none);
  gate.on_enqueued();
  gate.on_written();
  EXPECT_EQ(gate.try_resume_by_barrier(1, 100), Resume::by_barrier);
  // The same traffic never resumes a drain park.
  Gate draining;
  enqueue(draining, 2);
  draining.park_drain();
  for (int index = 0; index < 100; ++index) {
    draining.on_enqueued();
    draining.on_written();
    EXPECT_EQ(draining.try_resume_by_barrier(1, 100), Resume::none);
  }
  EXPECT_TRUE(draining.parked());
}

TEST(ReadResumeGate, ABarrierCanNeverBeMetWithoutWrites) {
  // Flow control: a client that does not read completes no write, so the read stays parked.
  Gate gate;
  enqueue(gate, 4);
  gate.park_barrier();
  for (int index = 0; index < 1000; ++index) {
    EXPECT_EQ(gate.try_resume_by_barrier(0, 0), Resume::none);
  }
  EXPECT_TRUE(gate.parked());
}

TEST(ReadResumeGate, TheBacklogGuardHoldsTheReadAndIsInclusiveAtTheLimit) {
  Gate gate;
  enqueue(gate, 1);
  gate.park_barrier();
  gate.on_written();
  EXPECT_EQ(gate.try_resume_by_barrier(Gate::kBacklogFrames + 1, 0), Resume::blocked_by_backlog);
  EXPECT_EQ(gate.try_resume_by_barrier(0, Gate::kBacklogBytes + 1), Resume::blocked_by_backlog);
  EXPECT_TRUE(gate.parked());
  EXPECT_EQ(gate.try_resume_by_barrier(Gate::kBacklogFrames, Gate::kBacklogBytes),
            Resume::by_barrier);
}

TEST(ReadResumeGate, ADrainParkResumesOnlyWhenTheQueueEmptied) {
  Gate gate;
  enqueue(gate, 2);
  gate.park_drain();
  gate.on_written();
  gate.on_written();
  EXPECT_EQ(gate.try_resume_by_barrier(0, 0), Resume::none);
  EXPECT_EQ(gate.try_resume_by_drain(), Resume::by_drain);
  EXPECT_FALSE(gate.parked());
  EXPECT_EQ(gate.try_resume_by_drain(), Resume::none);
}

TEST(ReadResumeGate, AResumeClearsTheParkSoTheDrainBranchCannotReadASecondTime) {
  Gate gate;
  enqueue(gate, 1);
  gate.park_barrier();
  gate.on_written();
  ASSERT_EQ(gate.try_resume_by_barrier(0, 0), Resume::by_barrier);
  EXPECT_EQ(gate.try_resume_by_drain(), Resume::none);
  EXPECT_EQ(gate.try_resume_by_barrier(0, 0), Resume::none);
  // A later park arms the gate again with a new barrier.
  enqueue(gate, 1);
  gate.park_barrier();
  EXPECT_EQ(gate.try_resume_by_barrier(0, 0), Resume::none);
  gate.on_written();
  EXPECT_EQ(gate.try_resume_by_barrier(0, 0), Resume::by_barrier);
}

TEST(ReadResumeGate, TheDrainFallbackStillResumesABarrierParkWhenTheQueueEmpties) {
  Gate gate;
  enqueue(gate, 2);
  gate.park_barrier();
  EXPECT_EQ(gate.try_resume_by_drain(), Resume::by_drain);
}

TEST(ReadResumeGate, ParkChoiceFollowsPresenceClosedStateAndOwnFrames) {
  // Handshake/welcome and the map-move request (presence reset before the park decision): drain.
  EXPECT_EQ(Gate::choose_park(false, false, 3), Park::drain);
  // Steady state with frames of this request: barrier. Republished presence is a new decision.
  EXPECT_EQ(Gate::choose_park(false, true, 1), Park::barrier);
  // Nothing enqueued by this request (e.g. the enqueue closed the connection): never a barrier.
  EXPECT_EQ(Gate::choose_park(false, true, 0), Park::drain);
  // A closed connection never parks.
  EXPECT_EQ(Gate::choose_park(true, true, 3), Park::none);
  EXPECT_EQ(Gate::choose_park(true, false, 0), Park::none);
}

TEST(ReadResumeGate, CountersFollowEnqueueAndWriteNotDequeue) {
  Gate gate;
  enqueue(gate, 3);
  EXPECT_EQ(gate.enqueued(), 3u);
  EXPECT_EQ(gate.written(), 0u);
  gate.on_written();
  EXPECT_EQ(gate.written(), 1u);
}

} // namespace
} // namespace chronicles
