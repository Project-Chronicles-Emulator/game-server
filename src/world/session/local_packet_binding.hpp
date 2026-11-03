#pragma once

#include "protocol/gameplay_packet.hpp"
#include "protocol/protocol.hpp"
#include "world/session/gathering_view_identity.hpp"

#include <functional>

namespace chronicles {

// The view check and encoder must use weak recipient ownership. The plan is evaluated without
// the codec lock, and a changed view is checked again before the frame reaches the wire queue.
using GatheringViewCheck = std::function<bool(const GatheringViewIdentity &)>;
using GatheringPacketEncoder = std::function<Frame(std::uint16_t, const nlohmann::json &)>;
using GatheringQueueGuard = std::function<std::shared_ptr<GatheringViewState::QueueGuard>()>;

void bind_local_gathering_packet(Frame &frame, const GatheringViewIdentity &view,
                                 GatheringViewCheck current,
                                 GatheringPacketEncoder encode,
                                 GatheringQueueGuard acquire_view);

} // namespace chronicles
