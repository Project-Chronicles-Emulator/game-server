#include "world/features/expression/expression_feature.hpp"

#include <iterator>
#include <stdexcept>
#include <utility>

#include <spdlog/spdlog.h>

namespace chronicles::expression {
namespace {

constexpr char kGroundMoveMode[] = "MoveMode_Normal";
constexpr char kCrouchingMoveMode[] = "MoveMode_Crouching";

} // namespace

ExpressionFeature::ExpressionFeature() : catalog_(std::make_shared<const EmotionCatalog>()) {
}

ExpressionFeature::ExpressionFeature(std::shared_ptr<const EmotionCatalog> catalog)
    : catalog_(std::move(catalog)) {
  if (!catalog_) {
    throw std::invalid_argument("ExpressionFeature requires an emotion catalog");
  }
}

void ExpressionFeature::load_catalog(ClientCatalog &catalog) {
  catalog_ = std::make_shared<const EmotionCatalog>(EmotionCatalog::load(catalog));
}

std::unique_ptr<ExpressionSession>
ExpressionFeature::create_session(ExpressionSessionContext &context) {
  return std::make_unique<ExpressionSession>(*this, context);
}

std::optional<GameplayPacket>
ExpressionFeature::late_viewer_notification(const nlohmann::json &unit_add_message) const {
  return selfie_registry_.late_viewer_notification(unit_add_message);
}

ExpressionSession::ExpressionSession(ExpressionFeature &feature, ExpressionSessionContext &context)
    : feature_(feature), context_(context) {
}

PlayerExpression &ExpressionSession::player() {
  if (!player_) {
    const auto progress = context_.expression_progress();
    player_.emplace(feature_.catalog_, context_.expression_player_unit_id(),
                    progress.owned_emotions);
  }
  return *player_;
}

bool ExpressionSession::handle(FrameContext &frame) {
  const auto *request = frame.request();
  if (!request || !context_.expression_has_character() ||
      !PlayerExpression::handles(request->opcode)) {
    return false;
  }

  auto packets = receive(*request, frame.party_position);
  frame.session_packets.insert(frame.session_packets.end(),
                               std::make_move_iterator(packets.begin()),
                               std::make_move_iterator(packets.end()));
  return true;
}

std::vector<GameplayPacket>
ExpressionSession::receive(const WorldGameplayRequest &request,
                           const std::optional<WorldPosition> &position) {
  const auto blockers = context_.expression_blockers();
  const bool ground_mode =
      last_move_mode_ == kGroundMoveMode || last_move_mode_ == kCrouchingMoveMode;

  ExpressionContext conditions;
  conditions.position = position;
  conditions.dead = blockers.dead;
  conditions.using_skill = blockers.using_skill;
  conditions.interacting = blockers.interacting;
  conditions.off_ground = blockers.riding || !ground_mode;

  auto &expression = player();
  auto outcome = expression.receive(request.opcode, request.message, conditions,
                                    context_.expression_now_ticks());
  if (outcome.emotion_result && *outcome.emotion_result != x5::ec::Success) {
    spdlog::info("Native world emote refused connection={} ec={}",
                 context_.expression_connection_id(),
                 x5::ec::EErrorCode_Name(*outcome.emotion_result));
  }
  if (request.opcode == kSelfieModeRequestOpcode && !context_.expression_closed()) {
    feature_.selfie_registry_.set(context_.expression_player_unit_id(), expression.selfie_active());
  }

  context_.deliver_expression_observers(outcome.observers);
  return std::move(outcome.own);
}

void ExpressionSession::observe_movement(const std::optional<nlohmann::json> &movement) {
  if (movement) {
    last_move_mode_ = movement->value("mode", std::string(kGroundMoveMode));
  }
}

void ExpressionSession::on_enter_world(bool entered_world,
                                       std::vector<Frame> &pending_notifications) {
  if (entered_world && context_.expression_has_character() && !snapshot_sent_) {
    snapshot_sent_ = true;
    try {
      const auto snapshot = player().snapshot();
      context_.append_expression_snapshot(snapshot, pending_notifications);
    } catch (const std::exception &error) {
      spdlog::error("Emotion snapshot failed connection={}: {}",
                    context_.expression_connection_id(), error.what());
    }
  }
}

void ExpressionSession::on_map_load_complete() {
  last_move_mode_ = kGroundMoveMode;
  if (player_) {
    player_->reset_selfie();
  }
  feature_.selfie_registry_.set(context_.expression_player_unit_id(), false);
}

void ExpressionSession::on_leave_world() {
  if (context_.expression_has_character()) {
    feature_.selfie_registry_.set(context_.expression_player_unit_id(), false);
  }
}

std::vector<GameplayPacket> ExpressionSession::grant_emotion(int emotion_id) {
  const auto before = context_.expression_progress();
  auto grant = plan_emotion_grant(*feature_.catalog_, before, emotion_id);
  if (!grant.error.empty()) {
    context_.reply_to_expression_command("emotion_add refused: " + grant.error + ".");
    return {};
  }

  bool committed{};
  try {
    committed = context_.save_expression_progress(before, grant.after);
  } catch (const std::exception &error) {
    spdlog::warn("GM emotion_add commit rejected for character {}: {}",
                 context_.expression_character_id(), error.what());
  }
  if (!committed) {
    context_.reply_to_expression_command("emotion_add was not saved (see the server log).");
    return {};
  }

  context_.adopt_expression_progress(grant.after);
  player().adopt_grant(emotion_id);
  spdlog::info("GM emotion_add applied for character {} emotion={}",
               context_.expression_character_id(), emotion_id);
  context_.reply_to_expression_command("emotion_add applied.");
  return {std::move(grant.notification)};
}

std::optional<std::vector<GameplayPacket>>
ExpressionSession::dispatch_gm(const WorldGameplayRequest &request) {
  const auto command = parse_emotion_grant_command(request.message);
  if (!command.matched) {
    return std::nullopt;
  }

  if (!command.emotion_id) {
    context_.reply_to_expression_command("Usage: " + command.error);
    return std::vector<GameplayPacket>{};
  }

  return grant_emotion(*command.emotion_id);
}

} // namespace chronicles::expression
