#include "world/core/frame_router.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace chronicles {

FrameRouter::FrameRouter(std::vector<FrameStep> ordered_steps) {
  std::vector<std::uint16_t> declared_opcodes;
  steps_.reserve(ordered_steps.size());

  for (const auto &step : ordered_steps) {
    RegisteredStep registered;
    registered.operation = step.operation_;

    if (const auto observer = std::get_if<FrameObserver *>(&registered.operation)) {
      registered.name = (*observer)->name();
    } else {
      const auto handler = std::get<FrameRequestHandler *>(registered.operation);
      registered.name = handler->name();
      registered.handles_all_opcodes = handler->handles_all_opcodes();
      const auto opcodes = handler->opcodes();
      registered.opcodes.assign(opcodes.begin(), opcodes.end());

      if (registered.handles_all_opcodes && !registered.opcodes.empty()) {
        throw std::invalid_argument("Frame handler has both explicit and all-opcode routing: " +
                                    registered.name);
      }
      if (!registered.handles_all_opcodes && registered.opcodes.empty()) {
        throw std::invalid_argument("Frame handler has no declared opcodes: " + registered.name);
      }

      std::ranges::sort(registered.opcodes);
      const auto duplicate =
          std::adjacent_find(registered.opcodes.begin(), registered.opcodes.end());
      if (duplicate != registered.opcodes.end()) {
        throw std::invalid_argument("Frame handler declares a duplicate opcode: " +
                                    registered.name);
      }

      declared_opcodes.insert(declared_opcodes.end(), registered.opcodes.begin(),
                              registered.opcodes.end());
    }

    if (registered.name.empty()) {
      throw std::invalid_argument("Frame step must have a diagnostic name");
    }
    steps_.push_back(std::move(registered));
  }

  std::ranges::sort(declared_opcodes);
  const auto unique_end = std::unique(declared_opcodes.begin(), declared_opcodes.end());
  declared_opcodes.erase(unique_end, declared_opcodes.end());

  schedules_.reserve(declared_opcodes.size());
  for (const auto opcode : declared_opcodes) {
    schedules_.emplace(opcode, compile_schedule(opcode));
  }
  unknown_opcode_schedule_ = compile_schedule(std::nullopt);
}

FrameRouter::Schedule FrameRouter::compile_schedule(std::optional<std::uint16_t> opcode) const {
  Schedule schedule;
  schedule.reserve(steps_.size());

  for (std::size_t index = 0; index < steps_.size(); ++index) {
    const auto &step = steps_[index];
    const bool observer = std::holds_alternative<FrameObserver *>(step.operation);
    const bool matches_opcode = opcode && std::ranges::binary_search(step.opcodes, *opcode);
    if (observer || step.handles_all_opcodes || matches_opcode) {
      schedule.push_back(index);
    }
  }

  return schedule;
}

std::vector<std::string> FrameRouter::step_names() const {
  std::vector<std::string> names;
  names.reserve(steps_.size());
  for (const auto &step : steps_) {
    names.push_back(step.name);
  }

  return names;
}

FrameDispatchResult FrameRouter::dispatch(FrameContext &context) const {
  context.active_step_ = {};
  const bool had_request = context.request() != nullptr;
  const Schedule *schedule = &unknown_opcode_schedule_;
  if (const auto request = context.request()) {
    const auto matched = schedules_.find(request->opcode);
    if (matched != schedules_.end()) {
      schedule = &matched->second;
    }
  }

  for (const auto index : *schedule) {
    const auto &step = steps_[index];
    if (const auto observer = std::get_if<FrameObserver *>(&step.operation)) {
      context.active_step_ = step.name;
      (*observer)->observe(context);
    } else if (context.request()) {
      context.active_step_ = step.name;
      const auto handler = std::get<FrameRequestHandler *>(step.operation);
      if (handler->handle(context)) {
        context.request_.reset();
      }
    }
  }

  context.active_step_ = {};
  if (!had_request) {
    return FrameDispatchResult::no_request;
  }
  if (context.request()) {
    return FrameDispatchResult::unhandled;
  }

  return FrameDispatchResult::handled;
}

} // namespace chronicles
