#pragma once

#include "world/core/frame_context.hpp"
#include "world/features/expression/player_expression.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace chronicles {

struct Frame;

namespace expression {

struct ExpressionBlockers {
  bool dead{};
  bool using_skill{};
  bool interacting{};
  bool riding{};
};

// The composition implements only the capabilities expression uses. Other feature types
// and their lifetime stay outside this module; this is not a world/session service locator.
class ExpressionSessionContext {
public:
  virtual ~ExpressionSessionContext() = default;

  virtual bool expression_has_character() const = 0;
  virtual bool expression_closed() const = 0;
  virtual std::uint64_t expression_connection_id() const = 0;
  virtual std::int64_t expression_character_id() const = 0;
  virtual std::uint64_t expression_player_unit_id() const = 0;
  virtual std::int64_t expression_now_ticks() const = 0;

  // A tutorial supplies its current cached revision; other sessions load owned progress.
  virtual WorldProgress expression_progress() const = 0;
  virtual bool save_expression_progress(const WorldProgress &before,
                                        const WorldProgress &after) = 0;
  virtual void adopt_expression_progress(const WorldProgress &after) = 0;
  virtual ExpressionBlockers expression_blockers() const = 0;

  virtual void deliver_expression_observers(const std::vector<GameplayPacket> &packets) = 0;
  virtual void reply_to_expression_command(const std::string &message) = 0;

  // Preserve the existing codec critical section, including insertion into the frame sink.
  virtual void append_expression_snapshot(const GameplayPacket &packet,
                                          std::vector<Frame> &pending_notifications) = 0;
};

class ExpressionSession;

// One per server. Catalog loading remains at the old startup call position; registry reads
// are thread-safe and still happen directly after decorating the subject's UnitAdd message.
class ExpressionFeature final {
public:
  ExpressionFeature();

  explicit ExpressionFeature(std::shared_ptr<const EmotionCatalog> catalog);

  void load_catalog(ClientCatalog &catalog);

  std::unique_ptr<ExpressionSession> create_session(ExpressionSessionContext &context);

  std::optional<GameplayPacket>
  late_viewer_notification(const nlohmann::json &unit_add_message) const;

private:
  friend class ExpressionSession;

  std::shared_ptr<const EmotionCatalog> catalog_;
  SelfieRegistry selfie_registry_;
};

// Worker-owned player state. Construction does not load progress or construct PlayerExpression;
// first expression use or the entry snapshot retains that original lazy execution point.
class ExpressionSession final {
public:
  ExpressionSession(ExpressionFeature &feature, ExpressionSessionContext &context);

  bool handle(FrameContext &frame);

  void observe_movement(const std::optional<nlohmann::json> &movement);

  // The once-only bit is set before snapshot/encoding and remains set after failure.
  void on_enter_world(bool entered_world, std::vector<Frame> &pending_notifications);

  void on_map_load_complete();

  // Called on the original I/O close path: only the shared registry changes here.
  // Resetting worker-owned PlayerExpression from this hook would introduce a data race.
  void on_leave_world();

  std::vector<GameplayPacket> grant_emotion(int emotion_id);

  // Other commands return nullopt. Malformed emotion grants return an engaged empty result
  // without constructing the lazy player.
  std::optional<std::vector<GameplayPacket>> dispatch_gm(const WorldGameplayRequest &request);

private:
  PlayerExpression &player();

  std::vector<GameplayPacket> receive(const WorldGameplayRequest &request,
                                      const std::optional<WorldPosition> &position);

  ExpressionFeature &feature_;
  ExpressionSessionContext &context_;
  std::optional<PlayerExpression> player_;
  bool snapshot_sent_{};
  std::string last_move_mode_{"MoveMode_Normal"};
};

} // namespace expression
} // namespace chronicles
