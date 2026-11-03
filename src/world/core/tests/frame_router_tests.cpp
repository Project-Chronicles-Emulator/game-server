#include "world/core/frame_router.hpp"

#include <gtest/gtest.h>

#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace chronicles {
namespace {

constexpr std::uint16_t kSharedInteractionOpcode = 20100;
constexpr std::uint16_t kDifferentOpcode = 20101;
constexpr std::uint16_t kUnknownOpcode = 65000;

using Trace = std::vector<std::string>;

FrameContext request_context(std::uint16_t opcode = kSharedInteractionOpcode) {
  return FrameContext(opcode, WorldGameplayRequest{opcode, 17, nlohmann::json::object()});
}

class RecordingObserver final : public FrameObserver {
public:
  using Action = std::function<void(FrameContext &)>;

  RecordingObserver(std::string name, Trace &trace, Action action = {})
      : name_(std::move(name)), trace_(trace), action_(std::move(action)) {
  }

  std::string_view name() const noexcept override {
    return name_;
  }

  void observe(FrameContext &context) override {
    trace_.push_back(name_);
    if (action_) {
      action_(context);
    }
  }

private:
  std::string name_;
  Trace &trace_;
  Action action_;
};

class RecordingHandler final : public FrameRequestHandler {
public:
  using Action = std::function<bool(FrameContext &)>;

  RecordingHandler(std::string name, std::vector<std::uint16_t> opcodes, Trace &trace,
                   Action action, bool all_opcodes = false)
      : name_(std::move(name)), opcodes_(std::move(opcodes)), trace_(trace),
        action_(std::move(action)), all_opcodes_(all_opcodes) {
  }

  std::string_view name() const noexcept override {
    return name_;
  }

  std::span<const std::uint16_t> opcodes() const noexcept override {
    return opcodes_;
  }

  bool handles_all_opcodes() const noexcept override {
    return all_opcodes_;
  }

  bool handle(FrameContext &context) override {
    trace_.push_back(name_);
    return action_(context);
  }

private:
  std::string name_;
  std::vector<std::uint16_t> opcodes_;
  Trace &trace_;
  Action action_;
  bool all_opcodes_;
};

bool decline(FrameContext &) {
  return false;
}

bool consume(FrameContext &) {
  return true;
}

TEST(FrameRouter, KeepsSameOpcodeHandlersAndObserversInDeclarationOrder) {
  Trace trace;
  RecordingObserver before("before", trace);
  RecordingHandler housing("housing", {kSharedInteractionOpcode}, trace, decline);
  RecordingObserver between("between", trace);
  RecordingHandler unrelated("unrelated", {kDifferentOpcode}, trace, consume);
  RecordingHandler statue("statue", {kSharedInteractionOpcode}, trace, decline);
  RecordingObserver after("after", trace);
  FrameRouter router({FrameStep::observer(before), FrameStep::handler(housing),
                      FrameStep::observer(between), FrameStep::handler(unrelated),
                      FrameStep::handler(statue), FrameStep::observer(after)});
  auto context = request_context();

  EXPECT_EQ(router.dispatch(context), FrameDispatchResult::unhandled);
  EXPECT_EQ(trace, (Trace{"before", "housing", "between", "statue", "after"}));
  ASSERT_NE(context.request(), nullptr);
  EXPECT_EQ(context.request()->sequence, 17);
}

TEST(FrameRouter, ConsumptionSkipsLaterHandlersButKeepsLaterObservers) {
  Trace trace;
  RecordingHandler housing("housing", {kSharedInteractionOpcode}, trace, consume);
  RecordingObserver sight("sight", trace, [](FrameContext &context) {
    EXPECT_EQ(context.request(), nullptr);
    context.session_packets.push_back({10, {{"kind", "sight"}}});
  });
  RecordingHandler statue("statue", {kSharedInteractionOpcode}, trace, consume);
  RecordingObserver finish("finish", trace);
  FrameRouter router({FrameStep::handler(housing), FrameStep::observer(sight),
                      FrameStep::handler(statue), FrameStep::observer(finish)});
  auto context = request_context();

  EXPECT_EQ(router.dispatch(context), FrameDispatchResult::handled);
  EXPECT_EQ(trace, (Trace{"housing", "sight", "finish"}));
  ASSERT_EQ(context.session_packets.size(), 1);
  EXPECT_EQ(context.session_packets.front().opcode, 10);
  EXPECT_TRUE(context.active_step().empty());
}

TEST(FrameRouter, GuardedObserverSeesConsumptionAtItsExistingPosition) {
  Trace trace;
  RecordingObserver before("before", trace, [&trace](FrameContext &context) {
    if (context.request()) {
      trace.push_back("before_has_request");
    }
  });
  RecordingHandler handler("handler", {kSharedInteractionOpcode}, trace, consume);
  RecordingObserver after("after", trace, [&trace](FrameContext &context) {
    if (context.request()) {
      trace.push_back("after_has_request");
    }
  });
  FrameRouter router(
      {FrameStep::observer(before), FrameStep::handler(handler), FrameStep::observer(after)});
  auto context = request_context();

  router.dispatch(context);

  EXPECT_EQ(trace, (Trace{"before", "before_has_request", "handler", "after"}));
}

TEST(FrameRouter, UnknownOpcodeReachesOnlyObserversAndExplicitFallback) {
  Trace trace;
  RecordingHandler known("known", {kSharedInteractionOpcode}, trace, consume);
  RecordingObserver before_fallback("before_fallback", trace);
  RecordingHandler fallback("tutorial", {}, trace, consume, true);
  RecordingObserver after_fallback("after_fallback", trace);
  FrameRouter router({FrameStep::handler(known), FrameStep::observer(before_fallback),
                      FrameStep::handler(fallback), FrameStep::observer(after_fallback)});
  auto context = request_context(kUnknownOpcode);

  EXPECT_EQ(router.dispatch(context), FrameDispatchResult::handled);
  EXPECT_EQ(trace, (Trace{"before_fallback", "tutorial", "after_fallback"}));
}

TEST(FrameRouter, UnknownOpcodeWithoutFallbackRemainsUnhandled) {
  Trace trace;
  RecordingObserver observer("observer", trace);
  RecordingHandler known("known", {kSharedInteractionOpcode}, trace, consume);
  FrameRouter router({FrameStep::observer(observer), FrameStep::handler(known)});
  auto context = request_context(kUnknownOpcode);

  EXPECT_EQ(router.dispatch(context), FrameDispatchResult::unhandled);
  EXPECT_EQ(trace, (Trace{"observer"}));
  ASSERT_NE(context.request(), nullptr);
  EXPECT_EQ(context.request()->opcode, kUnknownOpcode);
}

TEST(FrameRouter, NonConsumingBroadHandlerCanProducePacketsForUnknownOpcode) {
  Trace trace;
  RecordingHandler timed_tail(
      "timed_tail", {}, trace,
      [](FrameContext &context) {
        context.session_packets.push_back({27, {{"timed", true}}});
        context.action_deadline = 1234;
        return false;
      },
      true);
  RecordingObserver after("after", trace,
                          [](FrameContext &context) { EXPECT_NE(context.request(), nullptr); });
  FrameRouter router({FrameStep::handler(timed_tail), FrameStep::observer(after)});
  auto context = request_context(kUnknownOpcode);

  EXPECT_EQ(router.dispatch(context), FrameDispatchResult::unhandled);
  EXPECT_EQ(trace, (Trace{"timed_tail", "after"}));
  ASSERT_NE(context.request(), nullptr);
  EXPECT_EQ(context.request()->opcode, kUnknownOpcode);
  ASSERT_EQ(context.session_packets.size(), 1);
  EXPECT_EQ(context.session_packets.front().opcode, 27);
  EXPECT_EQ(context.action_deadline, 1234);
}

TEST(FrameRouter, BroadHandlerRetainsItsPositionOnKnownRoutes) {
  Trace trace;
  RecordingHandler first("first", {kSharedInteractionOpcode}, trace, decline);
  RecordingHandler broad("broad", {}, trace, decline, true);
  RecordingObserver middle("middle", trace);
  RecordingHandler last("last", {kSharedInteractionOpcode}, trace, consume);
  FrameRouter router({FrameStep::handler(first), FrameStep::handler(broad),
                      FrameStep::observer(middle), FrameStep::handler(last)});
  auto context = request_context();

  EXPECT_EQ(router.dispatch(context), FrameDispatchResult::handled);
  EXPECT_EQ(trace, (Trace{"first", "broad", "middle", "last"}));
}

TEST(FrameRouter, FrameWithoutGameplayRequestStillRunsEveryObserver) {
  Trace trace;
  RecordingObserver first("first", trace);
  RecordingHandler known("known", {kSharedInteractionOpcode}, trace, consume);
  RecordingHandler broad("broad", {}, trace, consume, true);
  RecordingObserver last("last", trace);
  FrameRouter router({FrameStep::observer(first), FrameStep::handler(known),
                      FrameStep::handler(broad), FrameStep::observer(last)});
  FrameContext context(kSharedInteractionOpcode, std::nullopt);

  EXPECT_EQ(router.dispatch(context), FrameDispatchResult::no_request);
  EXPECT_EQ(trace, (Trace{"first", "last"}));
}

TEST(FrameRouter, PositionInvalidationDoesNotRequireRequestConsumption) {
  Trace trace;
  RecordingHandler move("event_move", {kSharedInteractionOpcode}, trace, [](FrameContext &context) {
    context.invalidate_positions();
    return false;
  });
  RecordingHandler next("next", {kSharedInteractionOpcode}, trace, [](FrameContext &context) {
    EXPECT_FALSE(context.gameplay_position);
    EXPECT_FALSE(context.party_position);
    EXPECT_NE(context.request(), nullptr);
    return true;
  });
  FrameRouter router({FrameStep::handler(move), FrameStep::handler(next)});
  auto context = request_context();
  context.gameplay_position = WorldPosition{10117, 10, 20, 30, 0};
  context.party_position = context.gameplay_position;

  EXPECT_EQ(router.dispatch(context), FrameDispatchResult::handled);
  EXPECT_EQ(trace, (Trace{"event_move", "next"}));
}

TEST(FrameRouter, HandlerFailurePreservesExceptionStepAndOriginalOpcode) {
  Trace trace;
  RecordingHandler failing(
      "housing", {kSharedInteractionOpcode}, trace,
      [](FrameContext &) -> bool { throw std::runtime_error("storage unavailable"); });
  RecordingObserver later("later", trace);
  FrameRouter router({FrameStep::handler(failing), FrameStep::observer(later)});
  auto context = request_context();

  try {
    router.dispatch(context);
    FAIL() << "Expected the original handler failure";
  } catch (const std::runtime_error &error) {
    EXPECT_STREQ(error.what(), "storage unavailable");
    EXPECT_EQ(context.active_step(), "housing");
    EXPECT_EQ(context.incoming_opcode(), kSharedInteractionOpcode);
    EXPECT_NE(context.request(), nullptr);
  }

  EXPECT_EQ(trace, (Trace{"housing"}));
}

TEST(FrameRouter, ObserverFailureAfterConsumptionStillHasOriginalOpcode) {
  Trace trace;
  RecordingHandler handled("handled", {kSharedInteractionOpcode}, trace, consume);
  RecordingObserver failing(
      "sight", trace, [](FrameContext &) { throw std::runtime_error("encoding unavailable"); });
  FrameRouter router({FrameStep::handler(handled), FrameStep::observer(failing)});
  auto context = request_context();

  EXPECT_THROW(router.dispatch(context), std::runtime_error);
  EXPECT_EQ(context.request(), nullptr);
  EXPECT_EQ(context.active_step(), "sight");
  EXPECT_EQ(context.incoming_opcode(), kSharedInteractionOpcode);
}

TEST(FrameRouter, CompiledScheduleCanBeReusedAcrossIndependentFrames) {
  Trace trace;
  RecordingHandler first("first", {kSharedInteractionOpcode}, trace, consume);
  RecordingHandler second("second", {kDifferentOpcode}, trace, consume);
  FrameRouter router({FrameStep::handler(first), FrameStep::handler(second)});
  auto first_context = request_context();
  auto second_context = request_context(kDifferentOpcode);
  auto unknown_context = request_context(kUnknownOpcode);

  EXPECT_EQ(router.dispatch(first_context), FrameDispatchResult::handled);
  EXPECT_EQ(router.dispatch(unknown_context), FrameDispatchResult::unhandled);
  EXPECT_EQ(router.dispatch(second_context), FrameDispatchResult::handled);
  EXPECT_EQ(trace, (Trace{"first", "second"}));
  EXPECT_NE(unknown_context.request(), nullptr);
}

TEST(FrameRouter, SupportsMultipleDeclaredOpcodesAndFullWireRange) {
  Trace trace;
  constexpr auto maximum_opcode = std::numeric_limits<std::uint16_t>::max();
  RecordingHandler handler("wire_edges", {maximum_opcode, 0}, trace, consume);
  FrameRouter router({FrameStep::handler(handler)});
  auto zero = request_context(0);
  auto maximum = request_context(maximum_opcode);
  auto unrelated = request_context(kSharedInteractionOpcode);

  EXPECT_EQ(router.dispatch(zero), FrameDispatchResult::handled);
  EXPECT_EQ(router.dispatch(maximum), FrameDispatchResult::handled);
  EXPECT_EQ(router.dispatch(unrelated), FrameDispatchResult::unhandled);
  EXPECT_EQ(trace, (Trace{"wire_edges", "wire_edges"}));
}

TEST(FrameRouter, UsesDecodedRequestOpcodeForRouting) {
  Trace trace;
  RecordingHandler incoming("incoming", {kDifferentOpcode}, trace, consume);
  RecordingHandler decoded("decoded", {kSharedInteractionOpcode}, trace, consume);
  FrameRouter router({FrameStep::handler(incoming), FrameStep::handler(decoded)});
  FrameContext context(kDifferentOpcode, WorldGameplayRequest{kSharedInteractionOpcode, 17, {}});

  EXPECT_EQ(router.dispatch(context), FrameDispatchResult::handled);
  EXPECT_EQ(trace, (Trace{"decoded"}));
  EXPECT_EQ(context.incoming_opcode(), kDifferentOpcode);
}

TEST(FrameRouter, RejectsUnnamedObserver) {
  Trace trace;
  RecordingObserver observer("", trace);

  EXPECT_THROW(FrameRouter({FrameStep::observer(observer)}), std::invalid_argument);
}

TEST(FrameRouter, RejectsUnnamedHandler) {
  Trace trace;
  RecordingHandler handler("", {kSharedInteractionOpcode}, trace, consume);

  EXPECT_THROW(FrameRouter({FrameStep::handler(handler)}), std::invalid_argument);
}

TEST(FrameRouter, RejectsEmptyOpcodeSetWithoutExplicitBroadRouting) {
  Trace trace;
  RecordingHandler handler("empty", {}, trace, consume);

  EXPECT_THROW(FrameRouter({FrameStep::handler(handler)}), std::invalid_argument);
}

TEST(FrameRouter, RejectsDuplicateOpcodeWithinOneHandler) {
  Trace trace;
  RecordingHandler handler("duplicate", {kSharedInteractionOpcode, kSharedInteractionOpcode}, trace,
                           consume);

  EXPECT_THROW(FrameRouter({FrameStep::handler(handler)}), std::invalid_argument);
}

TEST(FrameRouter, RejectsAmbiguousBroadAndExplicitOpcodeDeclaration) {
  Trace trace;
  RecordingHandler handler("ambiguous", {kSharedInteractionOpcode}, trace, consume, true);

  EXPECT_THROW(FrameRouter({FrameStep::handler(handler)}), std::invalid_argument);
}

TEST(FrameRouter, EmptyPipelineLeavesRequestUnhandled) {
  FrameRouter router({});
  auto context = request_context();

  EXPECT_EQ(router.dispatch(context), FrameDispatchResult::unhandled);
  EXPECT_NE(context.request(), nullptr);
  EXPECT_TRUE(context.active_step().empty());
}

static_assert(std::is_same_v<decltype(std::declval<FrameContext &>().request()),
                             const WorldGameplayRequest *>);

} // namespace
} // namespace chronicles
