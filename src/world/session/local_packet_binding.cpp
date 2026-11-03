#include "world/session/local_packet_binding.hpp"
#include "world/session/local_packet_memory.hpp"

#include <memory>
#include <stdexcept>
#include <utility>

namespace chronicles {

void bind_local_gathering_packet(Frame &frame, const GatheringViewIdentity &view,
                                 GatheringViewCheck current, GatheringPacketEncoder encode,
                                 GatheringQueueGuard acquire_view) {
  if (!frame.local_plan) {
    if (frame.local_message) {
      throw std::logic_error("Gathering frame message has no local plan");
    }
    return;
  }
  if (!frame.local_message || !current || !encode || !acquire_view) {
    throw std::logic_error("Gathering frame binding is incomplete");
  }

  auto plan = std::move(frame.local_plan);
  auto original = std::move(frame.local_message);
  const auto opcode = frame.opcode();
  frame.prepare_for_enqueue = [plan = std::move(plan), original = std::move(original), view,
                               current = std::move(current), encode = std::move(encode),
                               acquire_view = std::move(acquire_view), opcode](Frame &queued) {
    if (!view.map_id || *view.map_id != plan->expected_map() ||
        view.copy_channel != plan->expected_channel() || !current(view)) {
      return false;
    }
    auto ordering = plan->begin_ordered_delivery();
    if (!ordering) {
      return false;
    }
    if (!view.map_id || *view.map_id != plan->expected_map() ||
        view.copy_channel != plan->expected_channel() || !current(view)) {
      return false;
    }

    auto message = *original;
    const auto decision = plan->prepare(message);
    if (decision == LocalPacketDecision::drop || !current(view)) {
      return false;
    }
    if (decision != LocalPacketDecision::keep && decision != LocalPacketDecision::replace) {
      throw std::logic_error("Unknown gathering packet preparation decision");
    }
    // The codec has its own mutex. Never acquire it while the source publication
    // domain is held: map transitions may acquire those locks in the other order.
    ordering.reset();
    if (decision == LocalPacketDecision::replace) {
      std::size_t reconstructed_bytes = 0;
      charge_local_json(message, 0, reconstructed_bytes);
      auto replacement = encode(opcode, message);
      queued.bytes = std::move(replacement.bytes);
      queued.shared = std::move(replacement.shared);
    }
    ordering = plan->begin_ordered_delivery();
    if (!ordering || !current(view)) {
      return false;
    }
    auto confirmed = *original;
    const auto confirmed_decision = plan->prepare(confirmed);
    if (confirmed_decision != decision || confirmed != message) {
      return false;
    }
    auto view_guard = acquire_view();
    if (!view_guard) {
      return false;
    }
    const auto sector = plan->initial_sector();
    if (sector) {
      queued.after_enqueue = [view_guard] {
        view_guard->mark_ready();
      };
    }
    struct QueueScope {
      std::shared_ptr<void> ordering;
      std::shared_ptr<GatheringViewState::QueueGuard> view;
    };
    queued.enqueue_scope = std::make_shared<QueueScope>(
        QueueScope{std::move(ordering), std::move(view_guard)});
    return true;
  };
}

} // namespace chronicles
