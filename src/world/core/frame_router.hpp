#pragma once

#include "world/core/frame_context.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace chronicles {

class FrameObserver {
public:
  virtual ~FrameObserver() = default;

  virtual std::string_view name() const noexcept = 0;
  virtual void observe(FrameContext &context) = 0;
};

class FrameRequestHandler {
public:
  virtual ~FrameRequestHandler() = default;

  virtual std::string_view name() const noexcept = 0;
  virtual std::span<const std::uint16_t> opcodes() const noexcept = 0;
  virtual bool handle(FrameContext &context) = 0;

  // A broad tutorial fallback must be declared explicitly at its current position in
  // the composition. It is not inferred from an empty or incomplete opcode list.
  virtual bool handles_all_opcodes() const noexcept {
    return false;
  }
};

class FrameStep {
public:
  static FrameStep observer(FrameObserver &observer) {
    return FrameStep(&observer);
  }

  static FrameStep handler(FrameRequestHandler &handler) {
    return FrameStep(&handler);
  }

private:
  friend class FrameRouter;

  using Operation = std::variant<FrameObserver *, FrameRequestHandler *>;

  explicit FrameStep(Operation operation) : operation_(operation) {
  }

  Operation operation_;
};

enum class FrameDispatchResult { no_request, unhandled, handled };

// Construct once after composing the session. Observers and handlers are borrowed and
// must outlive the router; the router must outlive contexts carrying failure attribution.
// Calls belong on the existing session worker strand. No mutex or task scheduling lives here.
class FrameRouter {
public:
  explicit FrameRouter(std::vector<FrameStep> ordered_steps);

  // Call once per context. Observers retain their interleaved positions and keep running
  // after consumption. Exceptions propagate unchanged for the session's rejection catch.
  FrameDispatchResult dispatch(FrameContext &context) const;

  // Diagnostic names of every step in composition order, for tests that pin that order.
  std::vector<std::string> step_names() const;

private:
  struct RegisteredStep {
    std::string name;
    FrameStep::Operation operation;
    std::vector<std::uint16_t> opcodes;
    bool handles_all_opcodes{};
  };

  using Schedule = std::vector<std::size_t>;

  Schedule compile_schedule(std::optional<std::uint16_t> opcode) const;

  std::vector<RegisteredStep> steps_;
  std::unordered_map<std::uint16_t, Schedule> schedules_;
  Schedule unknown_opcode_schedule_;
};

} // namespace chronicles
