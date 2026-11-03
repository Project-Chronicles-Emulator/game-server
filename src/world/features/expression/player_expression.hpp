#pragma once

#include "ec.generated.pb.h"
#include "protocol/gameplay_packet.hpp"
#include "persistence/character_state_types.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

namespace chronicles {

class ClientCatalog;

// Emotes (EmotionExecReq), emotion ownership and selfie mode.
//
// Evidence (docs/emotes-and-selfie.md, dumps/ida_research/expression-20261026):
// - The client sends EmotionExecReq{emotion_cms_id, pos, yaw} only for an emotion in its owned
//   set and outside EmotionCooltimeSec measured from its previous request. It plays its own
//   emote only after EmotionExecRes succeeds; an error code shows its text and cancels it.
// - EmotionExecNoti and SelfieModeNoti are ignored for the local player, so they address the
//   other players who see the unit. Official captures show EmotionExecNoti from other players.
// - The client seeds every Basic Emotion row as owned (Candidate: category byte); the server
//   lists only additional emotions. Official WorldCharEmotionsNoti snapshots are empty and are
//   sent once per character entry.
// - The client enters selfie mode locally and reports SelfieModeReq{enter}; no reply exists.
// - PC2S_StartSocialMotionReq has no wrapper or sender in this client build and is not handled.
inline constexpr std::uint16_t kEmotionExecRequestOpcode = 20063;
inline constexpr std::uint16_t kSelfieModeRequestOpcode = 20064;
inline constexpr std::uint16_t kEmotionExecResponseOpcode = 122;
inline constexpr std::uint16_t kEmotionExecNotificationOpcode = 123;
inline constexpr std::uint16_t kSelfieModeNotificationOpcode = 124;
inline constexpr std::uint16_t kWorldCharEmotionsNotificationOpcode = 144;
inline constexpr std::uint16_t kEmotionsUpdateNotificationOpcode = 145;

// x5.cms.EX5EmotionCategory. Only Basic emotions are owned without a grant.
enum class EmotionCategory { none, basic, reward, paid };

struct EmotionDefinition {
  int id{};
  EmotionCategory category{EmotionCategory::none};
  // Emotion.targetAnimStanceKey is set: the emote loops in that stance (ids 13 and 14).
  bool looping{};
};

// Emotion rows and the shared emote cooldown. Immutable after loading.
class EmotionCatalog {
public:
  // GameVariable EmotionCooltimeSec in the exported catalog, in 100 ns ticks.
  static constexpr std::int64_t kDefaultCooldownTicks = 10'000'000;

  // An empty catalog knows no emotion: every EmotionExecReq is answered Emotion_InvalidData.
  EmotionCatalog() = default;

  // Reads Tables/Emotion.jsonl and GameVariable EmotionCooltimeSec. A missing table leaves the
  // catalog empty; a malformed row throws so a bad export fails at startup.
  static EmotionCatalog load(ClientCatalog &catalog);

  // Builds a catalog from Emotion rows in the export's text format.
  static EmotionCatalog from_rows(const std::vector<nlohmann::json> &rows,
                                  std::int64_t cooldown_ticks = kDefaultCooldownTicks);

  const EmotionDefinition *find(int emotion_id) const;

  std::int64_t cooldown_ticks() const;

  std::size_t size() const;

private:
  std::unordered_map<int, EmotionDefinition> definitions_;
  std::int64_t cooldown_ticks_{kDefaultCooldownTicks};
};

// True when `emotion_id` may be used: a Basic row, or a granted row the catalog still knows.
bool owns_emotion(const EmotionCatalog &catalog, const std::set<int> &granted_emotions,
                  int emotion_id);

// Session conditions sampled when an expression request arrives.
struct ExpressionContext {
  // The server's current position of the player; absent before map entry or after exit.
  std::optional<WorldPosition> position;
  bool dead{};
  // A skill is in progress.
  bool using_skill{};
  // A gathering, interaction or tutorial action is in progress.
  bool interacting{};
  // Riding, or the last movement was not on the ground (falling, climbing, parkour, swimming,
  // flying or gliding).
  bool off_ground{};
};

struct ExpressionOutcome {
  // Packets for the requesting client.
  std::vector<GameplayPacket> own;
  // Packets for every other client that currently sees the player's unit.
  std::vector<GameplayPacket> observers;
  // EmotionExecReq result (Success or the refusal code); absent for SelfieModeReq.
  std::optional<x5::ec::EErrorCode> emotion_result;
};

// Per-session emote and selfie state. Confined to the session worker strand.
class PlayerExpression {
public:
  // `granted_emotions` are the character's persisted non-Basic emotions.
  PlayerExpression(std::shared_ptr<const EmotionCatalog> catalog, std::uint64_t player_unit_id,
                   std::set<int> granted_emotions);

  static bool handles(std::uint16_t opcode);

  // `now` is the internal 100 ns monotonic tick.
  ExpressionOutcome receive(std::uint16_t opcode, const nlohmann::json &message,
                            const ExpressionContext &context, std::int64_t now);

  // PS2C_WorldCharEmotionsNoti listing the granted emotions (empty for Basic-only characters,
  // as in every official capture).
  GameplayPacket snapshot() const;

  // Records an emotion committed by plan_emotion_grant so later requests may use it.
  void adopt_grant(int emotion_id);

  bool selfie_active() const;

  // Leaves selfie mode without notifying anyone. Used when the player changes maps: the old
  // map's viewers have already removed the unit, and the client closes its selfie view.
  void reset_selfie();

private:
  ExpressionOutcome execute_emotion(const nlohmann::json &message, const ExpressionContext &context,
                                    std::int64_t now);

  ExpressionOutcome change_selfie(const nlohmann::json &message, const ExpressionContext &context);

  x5::ec::EErrorCode emotion_refusal(int emotion_id, const ExpressionContext &context,
                                     std::int64_t now) const;

  std::shared_ptr<const EmotionCatalog> catalog_;
  std::uint64_t player_unit_id_{};
  std::set<int> granted_emotions_;
  // Internal tick of the last accepted emote; the cooldown is measured from it.
  std::optional<std::int64_t> last_emotion_tick_;
  bool selfie_active_{};
};

struct EmotionGrant {
  // Empty on success. Otherwise a short reason that is safe to log and reply to a GM.
  std::string error;
  // `before` advanced by one revision with the emotion added; valid only on success.
  WorldProgress after;
  // PS2C_EmotionsUpdateNoti for the owner; valid only on success.
  GameplayPacket notification;
};

// Plans a grant of one non-Basic catalog emotion. No state changes; the caller commits `after`.
EmotionGrant plan_emotion_grant(const EmotionCatalog &catalog, const WorldProgress &before,
                                int emotion_id);

// Emulator GM text command "emotion_add <emotion_id>" (aliases add_emotion, emote_add), carried
// by PC2S_GmCmdTextReq. No official GM emotion command was recovered.
struct EmotionGrantCommand {
  // False when the text is another command; another GM parser may own it.
  bool matched{};
  // Set when matched and valid.
  std::optional<int> emotion_id;
  // Usage text when matched but invalid; never repeats request text.
  std::string error;
};

EmotionGrantCommand parse_emotion_grant_command(const nlohmann::json &gm_text_message);

// Units currently in selfie mode, shared by every connection, so a player who comes into view
// later also sees the selfie pose. Thread-safe; never calls back into sessions.
class SelfieRegistry {
public:
  void set(std::uint64_t unit_id, bool active);

  // SelfieModeNoti a new viewer needs after the PS2C_UnitAddNoti body `unit_add_message`, or
  // nothing when that unit is not in selfie mode.
  std::optional<GameplayPacket>
  late_viewer_notification(const nlohmann::json &unit_add_message) const;

private:
  mutable std::mutex mutex_;
  std::unordered_set<std::uint64_t> active_units_;
};

} // namespace chronicles
