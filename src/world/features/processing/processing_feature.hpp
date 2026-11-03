#pragma once

#include "world/core/frame_context.hpp"
#include "world/core/item_grant_commit.hpp"
#include "world/features/processing/processing.hpp"

namespace chronicles::processing {

class ProcessingSessionContext {
public:
  virtual ~ProcessingSessionContext() = default;

  virtual const WorldProgress *processing_tutorial_progress() const = 0;
  virtual const nlohmann::json *processing_player_stats() const = 0;
  virtual std::int64_t processing_monotonic_ticks() const = 0;
  virtual bool processing_other_action_busy() const = 0;
  // A crafting station placed at runtime, such as one on the housing lot the character stands
  // on. The default serves only the profile's map stations.
  virtual std::optional<ProcessingPlacement> processing_placed_station(std::uint64_t unit_id,
                                                                       int map_id) const;
};

// ProcessingData is loaded by the existing startup path. This owner binds the original
// ProcessingSession to its selected durable subject, current cache and frame dispatch.
class ProcessingFeature final {
public:
  explicit ProcessingFeature(std::shared_ptr<const ProcessingData> data);

  // Domain packet policy; composition retains the original expertise/items/crafts order.
  static GameplayPacket expertise_entry_snapshot();
  static GameplayPacket crafts_entry_snapshot(const std::set<int> &learned_crafts = {});

  bool enabled() const noexcept;
  std::unique_ptr<ProcessingSession> create_session(std::uint64_t player_unit_id,
                                                    std::shared_ptr<ProgressAccess> progress,
                                                    ProcessingSessionContext &context,
                                                    SessionEventBus &events);
  bool handle(ProcessingSession *session, FrameContext &frame, ProcessingSessionContext &context);

private:
  const std::shared_ptr<const ProcessingData> data_;
};

} // namespace chronicles::processing
