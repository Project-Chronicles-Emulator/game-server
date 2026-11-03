#include "world/features/guild/guild.hpp"

#include "data/client_catalog.hpp"
#include "common/clock_ticks.hpp"
#include "persistence/account_repository.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <exception>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace chronicles {
namespace {

using x5::ec::EErrorCode;

constexpr char kGuildConfiguration[] = "Guild configuration";

// Emulator request bounds, not recovered official limits. The client offers join levels
// and company positions from fixed option lists far below these values.
constexpr int kMaximumJoinLevel = 100;
constexpr int kMaximumJoinCompanyPosition = 100;
constexpr std::size_t kMaximumSearchNameBytes = 128;
constexpr std::size_t kMaximumPendingInvites = 1024;

// Catalog plausibility bounds. A value outside them indicates a corrupt or misread row.
constexpr std::int64_t kMaximumNameLengthBound = 64;
constexpr std::int64_t kMaximumDescriptionLengthBound = 4000;
constexpr std::int64_t kMaximumSubMasterBound = 100;
constexpr std::int64_t kMaximumCooldownHours = 24 * 365;
constexpr std::int64_t kMinimumInviteMillis = 1000;
constexpr std::int64_t kMaximumInviteMillis = 600000;
constexpr std::int64_t kMaximumPageSize = 100;
constexpr std::int64_t kMaximumGuildMembers = 1000;
constexpr std::int64_t kMaximumGuildExperience = std::numeric_limits<std::int32_t>::max();
constexpr std::int64_t kMaximumContribution = std::numeric_limits<std::int32_t>::max();
constexpr int kMaximumCompanyPositionBound = 100;

constexpr std::int64_t kTicksPerHour = 3600 * kEpochTicksPerSecond;

bool in_range(char32_t code_point, char32_t first, char32_t last) {
  return code_point >= first && code_point <= last;
}

bool ascii_alphanumeric(char32_t code_point) {
  return in_range(code_point, U'a', U'z') || in_range(code_point, U'A', U'Z') ||
         in_range(code_point, U'0', U'9');
}

bool hangul_syllable(char32_t code_point) {
  return in_range(code_point, U'\uAC00', U'\uD7A3');
}

bool ascii_whitespace(char32_t code_point) {
  return code_point == U' ' || code_point == U'\t' || code_point == U'\n' ||
         code_point == U'\r' || code_point == U'\f' || code_point == U'\v';
}

// GameVariable GuildNameRegex: ^[A-Za-z0-9가-힣\-_.]+$. The exported value escapes the hyphen
// twice, which a literal reading turns into the range \ to _; the evident intent is used.
bool guild_name_code_point(char32_t code_point) {
  return ascii_alphanumeric(code_point) || hangul_syllable(code_point) || code_point == U'-' ||
         code_point == U'_' || code_point == U'.';
}

// GameVariable GuildDescRegex: Latin letters and digits, Hangul syllables and jamo, CJK
// ideographs, kana, Latin-1 letters, Arabic and whitespace. Punctuation is not accepted.
bool guild_description_code_point(char32_t code_point) {
  return ascii_alphanumeric(code_point) || hangul_syllable(code_point) ||
         in_range(code_point, U'\u3131', U'\u3163') || in_range(code_point, U'\u4E00', U'\u9FFF') ||
         in_range(code_point, U'\u3040', U'\u309F') || in_range(code_point, U'\u30A0', U'\u30FF') ||
         in_range(code_point, U'\u00C0', U'\u00FF') || in_range(code_point, U'\u0600', U'\u06FF') ||
         ascii_whitespace(code_point);
}

// Decodes strict UTF-8 (no overlong forms, surrogates or values above U+10FFFF).
std::optional<std::vector<char32_t>> decode_utf8(std::string_view text) {
  std::vector<char32_t> code_points;
  std::size_t index = 0;
  while (index < text.size()) {
    const auto lead = static_cast<unsigned char>(text[index]);
    std::size_t length = 0;
    char32_t code_point = 0;
    char32_t minimum = 0;
    if (lead < 0x80) {
      length = 1;
      code_point = lead;
    } else if ((lead & 0xE0) == 0xC0) {
      length = 2;
      code_point = lead & 0x1F;
      minimum = 0x80;
    } else if ((lead & 0xF0) == 0xE0) {
      length = 3;
      code_point = lead & 0x0F;
      minimum = 0x800;
    } else if ((lead & 0xF8) == 0xF0) {
      length = 4;
      code_point = lead & 0x07;
      minimum = 0x10000;
    } else {
      return std::nullopt;
    }

    if (length > text.size() - index) {
      return std::nullopt;
    }
    for (std::size_t offset = 1; offset < length; ++offset) {
      const auto continuation = static_cast<unsigned char>(text[index + offset]);
      if ((continuation & 0xC0) != 0x80) {
        return std::nullopt;
      }
      code_point = (code_point << 6) | (continuation & 0x3F);
    }

    const bool overlong = length > 1 && code_point < minimum;
    const bool surrogate = in_range(code_point, 0xD800, 0xDFFF);
    if (overlong || surrogate || code_point > 0x10FFFF) {
      return std::nullopt;
    }

    code_points.push_back(code_point);
    index += length;
  }
  return code_points;
}

bool all_code_points(std::string_view text, bool (*allowed)(char32_t)) {
  const auto code_points = decode_utf8(text);
  if (!code_points) {
    return false;
  }
  return std::ranges::all_of(*code_points, allowed);
}

// GuildLevel rows must be contiguous from level 1 with positive, non-decreasing member
// limits; otherwise the evidenced defaults are kept.
std::optional<std::vector<GuildLevelRule>> load_level_rules(ClientCatalog &catalog) {
  std::vector<GuildLevelRule> levels;
  for (const auto &row : catalog.rows("GuildLevel")) {
    const auto level = catalog_integer(row, "level");
    const auto max_members = catalog_integer(row, "maxMemberCount");
    const auto required = catalog_integer(row, "requiredGuildExp");
    const auto expected_level = static_cast<std::int64_t>(levels.size()) + 1;
    if (!level || *level != expected_level || !max_members || *max_members <= 0 ||
        *max_members > kMaximumGuildMembers || !required || *required <= 0 ||
        *required > kMaximumGuildExperience) {
      return std::nullopt;
    }

    const auto limit = static_cast<std::size_t>(*max_members);
    if (!levels.empty() && limit < levels.back().max_members) {
      return std::nullopt;
    }
    levels.push_back({static_cast<int>(*level), static_cast<int>(*required), limit});
  }

  if (levels.empty()) {
    return std::nullopt;
  }
  return levels;
}

bool is_manager(GuildGrade grade) {
  return grade == GuildGrade::master || grade == GuildGrade::sub_master;
}

// Master first, then SubMasters, then Members; ties by join time and character ID.
bool roster_order(const GuildMemberRecord &left, const GuildMemberRecord &right) {
  if (left.grade != right.grade) {
    return static_cast<int>(left.grade) < static_cast<int>(right.grade);
  }
  if (left.joined_epoch_ticks != right.joined_epoch_ticks) {
    return left.joined_epoch_ticks < right.joined_epoch_ticks;
  }
  return left.char_id < right.char_id;
}

} // namespace

std::optional<std::size_t> utf8_code_points(std::string_view text) {
  const auto code_points = decode_utf8(text);
  if (!code_points) {
    return std::nullopt;
  }
  return code_points->size();
}

bool valid_guild_name_text(std::string_view name) {
  return !name.empty() && all_code_points(name, guild_name_code_point);
}

bool valid_guild_description_text(std::string_view description) {
  return all_code_points(description, guild_description_code_point);
}

GuildRules load_guild_rules(ClientCatalog &catalog) {
  GuildRules rules;
  if (catalog.has_table("GameVariable")) {
    const auto name_min = game_variable_integer(catalog, "GuildNameUnitLengthMin", 1,
                                                kMaximumNameLengthBound, kGuildConfiguration);
    const auto name_max = game_variable_integer(catalog, "GuildNameUnitLengthMax", 1,
                                                kMaximumNameLengthBound, kGuildConfiguration);
    if (name_min && name_max && *name_min <= *name_max) {
      rules.name_min_length = static_cast<std::size_t>(*name_min);
      rules.name_max_length = static_cast<std::size_t>(*name_max);
    }

    const auto description_max =
        game_variable_integer(catalog, "GuildDescUnitLengthMax", 1,
                              kMaximumDescriptionLengthBound, kGuildConfiguration);
    if (description_max) {
      rules.description_max_length = static_cast<std::size_t>(*description_max);
    }

    const auto sub_masters = game_variable_integer(catalog, "MaxGuildSubMasterCount", 0,
                                                   kMaximumSubMasterBound, kGuildConfiguration);
    if (sub_masters) {
      rules.max_sub_masters = static_cast<std::size_t>(*sub_masters);
    }

    const auto rejoin = game_variable_integer(catalog, "GuildRejoinableTimeHours", 0,
                                              kMaximumCooldownHours, kGuildConfiguration);
    if (rejoin) {
      rules.rejoin_cooldown = std::chrono::hours(*rejoin);
    }

    const auto recreate = game_variable_integer(catalog, "GuildRecreatableTimeHours", 0,
                                                kMaximumCooldownHours, kGuildConfiguration);
    if (recreate) {
      rules.recreate_cooldown = std::chrono::hours(*recreate);
    }

    const auto disband = game_variable_integer(catalog, "GuildDisbandableTimeHours", 0,
                                               kMaximumCooldownHours, kGuildConfiguration);
    if (disband) {
      rules.disband_cooldown = std::chrono::hours(*disband);
    }

    const auto invite = game_variable_integer(catalog, "GuildInviteTimeoutMillis",
                                              kMinimumInviteMillis, kMaximumInviteMillis,
                                              kGuildConfiguration);
    if (invite) {
      rules.invite_timeout = std::chrono::milliseconds(*invite);
    }

    const auto page_size = game_variable_integer(catalog, "GuildJoinableListCount", 1,
                                                 kMaximumPageSize, kGuildConfiguration);
    if (page_size) {
      rules.joinable_list_page_size = static_cast<std::size_t>(*page_size);
    }

    const auto creatable_position =
        game_variable_integer(catalog, "GuildCreatableCompanyPosition", 0,
                              kMaximumCompanyPositionBound, kGuildConfiguration);
    if (creatable_position) {
      rules.creatable_company_position = static_cast<int>(*creatable_position);
    }
  } else {
    spdlog::warn("{}: catalog has no GameVariable table; using defaults", kGuildConfiguration);
  }

  if (catalog.has_table("GuildLevel")) {
    auto levels = load_level_rules(catalog);
    if (levels) {
      rules.levels = std::move(*levels);
    } else {
      spdlog::warn("{}: GuildLevel rows are not contiguous positive member limits; "
                   "using defaults",
                   kGuildConfiguration);
    }
  } else {
    spdlog::warn("{}: catalog has no GuildLevel table; using defaults", kGuildConfiguration);
  }

  return rules;
}

GuildClock system_guild_clock() {
  return GuildClock{unix_epoch_ticks_now, std::chrono::steady_clock::now};
}

GuildService::GuildService(std::shared_ptr<GuildStore> store, GuildRules rules, int world_id,
                           GuildClock clock)
    : store_(std::move(store)), rules_(std::move(rules)), world_id_(world_id),
      clock_(std::move(clock)) {
  if (!store_) {
    throw std::invalid_argument("Guild service requires a store");
  }
  if (rules_.levels.empty()) {
    throw std::invalid_argument("Guild rules require at least one level");
  }
  if (rules_.character_experience_per_guild_experience <= 0) {
    throw std::invalid_argument("Guild experience conversion must be positive");
  }

  auto snapshot = store_->load(world_id_);
  std::lock_guard lock(mutex_);
  load_snapshot(std::move(snapshot));
  repair_loaded_guilds();
}

void GuildService::subscribe(GuildEventSubscriber subscriber) {
  std::lock_guard lock(mutex_);
  subscribers_.push_back(std::move(subscriber));
}

void GuildService::set_before_progression_notification_for_test(std::function<void()> hook) {
  std::lock_guard lock(mutex_);
  before_progression_notification_for_test_ = std::move(hook);
}

const GuildRules &GuildService::rules() const {
  return rules_;
}

int GuildService::world_id() const {
  return world_id_;
}

template <typename Operation> auto GuildService::transact(Operation &&operation) {
  std::vector<GuildEvent> events;
  auto result = [&] {
    std::lock_guard lock(mutex_);
    require_settled_quest_cache_locked();
    return operation(events);
  }();

  publish(events);
  return result;
}

GuildListResult GuildService::joinable(const GuildActor &actor, int start_index) {
  std::lock_guard lock(mutex_);
  require_authoritative_guild_cache_locked();
  return joinable_locked(actor, start_index);
}

GuildSearchResult GuildService::search(const GuildActor &actor, const std::string &name) {
  std::lock_guard lock(mutex_);
  require_authoritative_guild_cache_locked();
  return search_locked(actor, name);
}

GuildDetailResult GuildService::detail(const GuildActor &actor, std::int64_t guild_id) {
  std::lock_guard lock(mutex_);
  require_authoritative_guild_cache_locked();
  return detail_locked(actor, guild_id);
}

GuildCreateResult GuildService::create(const GuildActor &actor,
                                       const GuildCreateRequest &request) {
  return transact([&](std::vector<GuildEvent> &events) {
    return create_locked(actor, request, events);
  });
}

GuildManageResult GuildService::manage(const GuildActor &actor,
                                       const GuildManageRequest &request) {
  return transact([&](std::vector<GuildEvent> &events) {
    return manage_locked(actor, request, events);
  });
}

GuildMemberManageResult GuildService::manage_member(const GuildActor &actor,
                                                    std::int64_t guild_id,
                                                    std::int64_t target_char_id, int grade) {
  return transact([&](std::vector<GuildEvent> &events) {
    return manage_member_locked(actor, guild_id, target_char_id, grade, events);
  });
}

GuildJoinResult GuildService::join(const GuildActor &actor, std::int64_t guild_id) {
  return transact([&](std::vector<GuildEvent> &events) {
    return join_locked(actor, guild_id, events);
  });
}

GuildLeaveResult GuildService::leave(const GuildActor &actor, std::int64_t guild_id) {
  return transact([&](std::vector<GuildEvent> &events) {
    return leave_locked(actor, guild_id, events);
  });
}

GuildDisbandResult GuildService::disband(const GuildActor &actor, std::int64_t guild_id) {
  return transact([&](std::vector<GuildEvent> &events) {
    return disband_locked(actor, guild_id, events);
  });
}

GuildInviteResult GuildService::invite(const GuildActor &inviter, std::int64_t guild_id,
                                       const GuildActor &target) {
  return transact([&](std::vector<GuildEvent> &events) {
    return invite_locked(inviter, guild_id, target, events);
  });
}

EErrorCode GuildService::accept_invite(const GuildActor &invitee, std::int64_t guild_id,
                                       std::int64_t inviter_char_id) {
  return transact([&](std::vector<GuildEvent> &events) {
    return accept_invite_locked(invitee, guild_id, inviter_char_id, events);
  });
}

EErrorCode GuildService::reject_invite(const GuildActor &invitee, std::int64_t guild_id,
                                       std::int64_t inviter_char_id) {
  return transact([&](std::vector<GuildEvent> &events) {
    return reject_invite_locked(invitee, guild_id, inviter_char_id, events);
  });
}

void GuildService::refresh_member(const GuildActor &actor) {
  std::vector<GuildEvent> events;
  {
    std::lock_guard lock(mutex_);
    require_settled_quest_cache_locked();
    refresh_member_locked(actor, events);
  }

  publish(events);
}

GuildStatus GuildService::status(std::int64_t char_id) {
  std::lock_guard lock(mutex_);
  require_authoritative_guild_cache_locked();
  const auto history = history_of(char_id);

  GuildStatus status;
  status.last_leave_epoch_ticks = history.last_leave_epoch_ticks;
  status.last_disband_epoch_ticks = history.last_disband_epoch_ticks;

  const auto *entry = guild_of(char_id);
  if (entry) {
    status.guild_id = entry->record.id;
    status.guild_name = entry->record.name;
    status.guild_level = entry->record.level;
    status.max_members = member_limit(entry->record);
    status.grade = entry->members.at(char_id).grade;
  }
  return status;
}

std::vector<std::int64_t> GuildService::guild_mates(std::int64_t char_id) {
  std::lock_guard lock(mutex_);
  require_authoritative_guild_cache_locked();
  const auto *entry = guild_of(char_id);
  if (!entry) {
    return {};
  }

  std::vector<std::int64_t> mates;
  mates.reserve(entry->members.size());
  for (const auto &[member_id, member] : entry->members) {
    mates.push_back(member_id);
  }
  return mates;
}

std::vector<GuildMemberRecord> GuildService::members(std::int64_t guild_id) {
  std::lock_guard lock(mutex_);
  require_authoritative_guild_cache_locked();
  const auto *entry = find_guild(guild_id);
  if (!entry) {
    return {};
  }
  return roster(*entry);
}

bool GuildService::award_experience(std::int64_t char_id, int amount) {
  std::vector<GuildEvent> events;
  bool awarded = false;
  {
    std::lock_guard lock(mutex_);
    require_settled_quest_cache_locked();
    awarded = award_experience_locked(char_id, amount, events);
  }

  publish(events);
  return awarded;
}

void GuildService::require_settled_quest_cache_locked() const {
  if (uncertain_quest_effect_ || uncertain_progression_) {
    throw std::runtime_error("Guild changes are paused while progression is reconciled");
  }
}

void GuildService::require_authoritative_guild_cache_locked() const {
  if (uncertain_quest_effect_ ||
      (uncertain_progression_ && !progression_readback_complete_)) {
    throw std::runtime_error("Guild state is waiting for authoritative readback");
  }
}

void GuildService::install_quest_effect_snapshot_locked(const GuildQuestEffectResult &result) {
  if (!result.guild || !result.member) {
    throw std::runtime_error("Guild quest effect did not return its authoritative progression");
  }
  auto *entry = find_guild(result.guild->id);
  if (!entry) {
    throw std::runtime_error("Guild quest effect refers to an uncached guild");
  }
  const auto member = entry->members.find(result.member->char_id);
  if (member == entry->members.end()) {
    throw std::runtime_error("Guild quest effect refers to an uncached member");
  }
  entry->record.experience = result.guild->experience;
  entry->record.level = result.guild->level;
  member->second.contribution = result.member->contribution;
}

QuestEffectSettlementStatus GuildService::settle_quest_effect(
    const QuestServiceEffectEvent &event) {
  std::lock_guard lock(mutex_);
  if (uncertain_quest_effect_ || uncertain_progression_) {
    return QuestEffectSettlementStatus::retry;
  }
  if (event.world_id != world_id_ || event.effects.guild_experience < 0 ||
      event.effects.activity_experience < 0 ||
      (event.effects.guild_experience == 0 && event.effects.activity_experience == 0)) {
    return QuestEffectSettlementStatus::identity_mismatch;
  }
  try {
    auto current = store_->read_quest_effect(event);
    if (current.status == QuestEffectSettlementStatus::already_applied) {
      uncertain_quest_effect_ = event;
      install_quest_effect_snapshot_locked(current);
      uncertain_quest_effect_.reset();
      return current.status;
    }
    if (current.status != QuestEffectSettlementStatus::pending) {
      return current.status;
    }
    install_quest_effect_snapshot_locked(current);
    auto *entry = find_guild(event.effects.expected_guild_id);
    auto before_member = entry->members.at(event.character_id);
    auto after_member = before_member;
    auto next_remainder = current.activity_remainder;
    std::int64_t activity_points = 0;
    if (event.effects.activity_experience > 0) {
      const auto rate = event.effects.activity_guild_rate;
      if (rate <= 0 || next_remainder < 0 || next_remainder >= rate) {
        return QuestEffectSettlementStatus::identity_mismatch;
      }
      activity_points = event.effects.activity_experience / rate;
      const auto incoming_remainder = event.effects.activity_experience % rate;
      if (next_remainder >= rate - incoming_remainder) {
        next_remainder -= rate - incoming_remainder;
        ++activity_points;
      } else {
        next_remainder += incoming_remainder;
      }
    }
    const auto available = std::numeric_limits<std::int64_t>::max() - activity_points;
    const auto awarded = std::min(event.effects.guild_experience, available) + activity_points;
    const auto contribution_room = std::int64_t{kMaximumContribution} - before_member.contribution;
    after_member.contribution += static_cast<int>(
        std::min(awarded, contribution_room));
    const auto experience_room = std::numeric_limits<std::int64_t>::max() - entry->record.experience;
    const auto delta = std::min(awarded, experience_room);
    const auto after_guild = with_experience(entry->record, delta);

    // Retain the immutable event before entering the transaction: a thrown COMMIT may
    // have succeeded and no longer appear in the pending-event enumeration.
    uncertain_quest_effect_ = event;
    auto result = store_->settle_quest_effect(event, entry->record, before_member,
                                              after_guild, after_member,
                                              current.activity_remainder, next_remainder);
    if (result.status == QuestEffectSettlementStatus::applied ||
        result.status == QuestEffectSettlementStatus::already_applied) {
      install_quest_effect_snapshot_locked(result);
    }
    uncertain_quest_effect_.reset();
    return result.status;
  } catch (const std::exception &error) {
    spdlog::error("Guild quest effect {} requires reconciliation: {}", event.effect_id,
                  error.what());
    return QuestEffectSettlementStatus::retry;
  }
}

std::optional<QuestEffectReconciliation> GuildService::reconcile_uncertain_quest_effect() {
  std::lock_guard lock(mutex_);
  if (uncertain_progression_) {
    return QuestEffectReconciliation{QuestEffectSettlementStatus::retry, 0, {}, false};
  }
  if (!uncertain_quest_effect_) {
    return std::nullopt;
  }
  const auto character_id = uncertain_quest_effect_->character_id;
  const auto effect_id = uncertain_quest_effect_->effect_id;
  try {
    const auto result = store_->read_quest_effect(*uncertain_quest_effect_);
    if (result.status == QuestEffectSettlementStatus::already_applied ||
        result.status == QuestEffectSettlementStatus::pending) {
      install_quest_effect_snapshot_locked(result);
      uncertain_quest_effect_.reset();
    }
    return QuestEffectReconciliation{result.status, character_id, effect_id,
                                     result.event_completed};
  } catch (const std::exception &error) {
    spdlog::error("Guild quest effect reconciliation failed: {}", error.what());
    return QuestEffectReconciliation{QuestEffectSettlementStatus::retry, character_id, effect_id,
                                     false};
  }
}

bool GuildService::reconcile_uncertain_progression() {
  std::lock_guard reconciliation_lock(progression_reconciliation_mutex_);
  std::vector<GuildEvent> events;
  {
    std::lock_guard lock(mutex_);
    if (!uncertain_progression_) {
      return true;
    }
    progression_readback_complete_ = false;
    try {
      auto snapshot = store_->read_progression(
          world_id_, uncertain_progression_->guild_id, uncertain_progression_->character_id);
      auto *entry = find_guild(snapshot.guild.id);
      if (!entry) {
        throw std::runtime_error("Guild progression readback refers to an uncached guild");
      }
      if (uncertain_progression_->character_id) {
        const auto member = entry->members.find(*uncertain_progression_->character_id);
        if (!snapshot.member || member == entry->members.end()) {
          throw std::runtime_error("Guild progression readback refers to an uncached member");
        }
        member->second.contribution = snapshot.member->contribution;
        events.push_back(GuildMemberUpdated{member->second, recipients(*entry, 0)});
      }
      if (uncertain_progression_->settings_changed) {
        events.push_back(GuildInfoChanged{snapshot.guild, recipients(*entry, 0)});
      }
      events.push_back(GuildExperienceChanged{snapshot.guild.id, snapshot.guild.experience,
                                               snapshot.guild.level, false, recipients(*entry, 0)});
      using std::swap;
      swap(entry->record, snapshot.guild);
      progression_readback_complete_ = true;
    } catch (const std::exception &error) {
      spdlog::error("Guild progression readback failed: {}", error.what());
      return false;
    }
  }

  try {
    publish(events, true);
  } catch (const std::exception &error) {
    spdlog::error("Guild progression resynchronization failed: {}", error.what());
    return false;
  } catch (...) {
    spdlog::error("Guild progression resynchronization failed with an unknown error");
    return false;
  }
  std::lock_guard lock(mutex_);
  uncertain_progression_.reset();
  progression_readback_complete_ = false;
  return true;
}

void GuildService::resynchronize_quest_effect(std::int64_t character_id) {
  std::vector<GuildEvent> events;
  {
    std::lock_guard lock(mutex_);
    require_settled_quest_cache_locked();
    const auto *entry = guild_of(character_id);
    if (!entry) {
      return;
    }
    const auto recipients = this->recipients(*entry, 0);
    events.push_back(GuildMemberUpdated{entry->members.at(character_id), recipients});
    events.push_back(GuildExperienceChanged{entry->record.id, entry->record.experience,
                                             entry->record.level, false, recipients});
  }
  publish(events, true);
}

bool GuildService::record_character_experience(std::int64_t char_id, std::int64_t experience) {
  std::vector<GuildEvent> events;
  bool awarded = false;
  {
    std::lock_guard lock(mutex_);
    require_settled_quest_cache_locked();
    if (experience <= 0 || !guild_of(char_id)) {
      return false;
    }

    auto &remainder = experience_remainders_[char_id];
    const auto per_point = rules_.character_experience_per_guild_experience;
    const auto bounded =
        std::min(experience, std::numeric_limits<std::int64_t>::max() - remainder);
    const auto total = bounded + remainder;
    const auto next_remainder = total % per_point;
    const auto points = std::min<std::int64_t>(total / per_point, kMaximumGuildExperience);
    if (points == 0) {
      remainder = next_remainder;
      return false;
    }
    awarded = award_experience_locked(char_id, static_cast<int>(points), events);
    if (awarded) {
      remainder = next_remainder;
    }
  }

  publish(events);
  return awarded;
}

EErrorCode GuildService::gm_add_experience(std::int64_t guild_id, int delta) {
  std::vector<GuildEvent> events;
  EErrorCode error = x5::ec::Success;
  {
    std::lock_guard lock(mutex_);
    require_settled_quest_cache_locked();
    auto *entry = find_guild(guild_id);
    if (!entry) {
      return x5::ec::Guild_NotExist;
    }

    const auto updated = with_experience(entry->record, delta);
    if (!store_progression_locked(*entry, updated, events)) {
      error = x5::ec::Guild_FailedUpdate;
    }
  }

  publish(events);
  return error;
}

EErrorCode GuildService::gm_set_level(std::int64_t guild_id, int level) {
  std::vector<GuildEvent> events;
  EErrorCode error = x5::ec::Success;
  {
    std::lock_guard lock(mutex_);
    require_settled_quest_cache_locked();
    auto *entry = find_guild(guild_id);
    if (!entry) {
      return x5::ec::Guild_NotExist;
    }
    if (level < rules_.levels.front().level || level > rules_.levels.back().level) {
      return x5::ec::Guild_InvalidLevel;
    }

    auto updated = entry->record;
    updated.level = level;
    updated.experience = 0;
    if (!store_progression_locked(*entry, updated, events)) {
      error = x5::ec::Guild_FailedUpdate;
    }
  }

  publish(events);
  return error;
}

EErrorCode GuildService::gm_disband(std::int64_t guild_id) {
  std::vector<GuildEvent> events;
  {
    std::lock_guard lock(mutex_);
    require_settled_quest_cache_locked();
    auto *entry = find_guild(guild_id);
    if (!entry) {
      return x5::ec::Guild_NotExist;
    }

    // An operator disband starts no member cooldowns.
    try {
      store_->delete_guild(guild_id, {});
    } catch (const std::exception &error) {
      spdlog::error("GM disband of guild {} failed: {}", guild_id, error.what());
      return x5::ec::Guild_FailedUpdate;
    }
    remove_guild_locked(*entry, nullptr, events);
    spdlog::info("Guild {} disbanded by a game master", guild_id);
  }

  publish(events);
  return x5::ec::Success;
}

EErrorCode GuildService::gm_set_master(std::int64_t guild_id, std::int64_t char_id) {
  std::vector<GuildEvent> events;
  EErrorCode error = x5::ec::Success;
  {
    std::lock_guard lock(mutex_);
    require_settled_quest_cache_locked();
    auto *entry = find_guild(guild_id);
    if (!entry) {
      return x5::ec::Guild_NotExist;
    }
    const auto target = entry->members.find(char_id);
    if (target == entry->members.end()) {
      return x5::ec::Guild_NotExistSelectedGuildMember;
    }
    if (target->second.grade == GuildGrade::master) {
      return x5::ec::Guild_MemberGradeAlreadyChanged;
    }

    const auto current_master = std::ranges::find_if(entry->members, [](const auto &member) {
      return member.second.grade == GuildGrade::master;
    });
    if (current_master == entry->members.end()) {
      return x5::ec::Guild_InvalidGuildInfo;
    }
    // Reuse the transfer path; the operator acts with the current master's authority.
    const auto master_record = current_master->second;
    const auto target_record = target->second;
    error = change_grade(*entry, master_record, target_record, GuildGrade::master, events);
    for (auto &event : events) {
      if (auto *changed = std::get_if<GuildMemberGradeChanged>(&event)) {
        changed->recipients = recipients(*entry, 0);
      }
    }
  }

  publish(events);
  return error;
}

EErrorCode GuildService::gm_reset_cooldowns(std::int64_t char_id) {
  std::lock_guard lock(mutex_);
  require_settled_quest_cache_locked();
  if (char_id <= 0) {
    return x5::ec::Guild_InvalidChar;
  }

  GuildHistory cleared;
  cleared.char_id = char_id;
  try {
    store_->save_history(cleared);
  } catch (const std::exception &error) {
    spdlog::error("Guild cooldown reset for character {} failed: {}", char_id, error.what());
    return x5::ec::Guild_FailedUpdate;
  }
  histories_[char_id] = cleared;
  return x5::ec::Success;
}

bool GuildService::award_experience_locked(std::int64_t char_id, int amount,
                                           std::vector<GuildEvent> &events) {
  const auto guild_id = guild_by_member_.find(char_id);
  if (amount <= 0 || guild_id == guild_by_member_.end()) {
    return false;
  }
  auto &entry = guilds_.at(guild_id->second);

  const auto before_member = entry.members.at(char_id);
  auto member = before_member;
  member.contribution =
      static_cast<int>(std::min<std::int64_t>(member.contribution + std::int64_t{amount},
                                              kMaximumContribution));
  const auto updated = with_experience(entry.record, amount);
  const bool progression_changed = updated.experience != entry.record.experience ||
                                   updated.level != entry.record.level;
  const bool level_changed = updated.level != entry.record.level;
  auto member_event = GuildMemberUpdated{member, recipients(entry, 0)};
  auto progression_event = GuildExperienceChanged{updated.id, updated.experience, updated.level,
                                                   level_changed, recipients(entry, 0)};
  events.reserve(events.size() + (progression_changed ? 2 : 1));
  uncertain_progression_ = UncertainProgression{entry.record.id, char_id};
  progression_readback_complete_ = false;
  try {
    store_->commit_progression(entry.record, before_member, updated, member);
  } catch (const std::exception &error) {
    spdlog::error("Guild progression for character {} was not stored: {}", char_id,
                  error.what());
    return false;
  }
  entry.members.at(char_id).contribution = member.contribution;
  entry.record.experience = updated.experience;
  entry.record.level = updated.level;
  uncertain_progression_.reset();
  events.push_back(std::move(member_event));
  if (progression_changed) {
    events.push_back(std::move(progression_event));
  }
  return true;
}

GuildRecord GuildService::with_experience(GuildRecord guild, std::int64_t delta) const {
  const auto max_level = rules_.levels.back().level;
  auto experience = std::max<std::int64_t>(0, std::int64_t{guild.experience} + delta);
  while (guild.level < max_level) {
    const auto rule = std::ranges::find(rules_.levels, guild.level, &GuildLevelRule::level);
    if (rule == rules_.levels.end() || experience < rule->required_experience) {
      break;
    }
    experience -= rule->required_experience;
    ++guild.level;
  }

  // Like CharacterLevel's final row, the maximum level keeps no progress.
  if (guild.level >= max_level) {
    guild.level = max_level;
    experience = 0;
  }
  guild.experience = static_cast<int>(std::min(experience, kMaximumGuildExperience));
  return guild;
}

bool GuildService::store_progression_locked(GuildEntry &entry, const GuildRecord &updated,
                                            std::vector<GuildEvent> &events) {
  if (updated.experience == entry.record.experience && updated.level == entry.record.level) {
    return true;
  }

  const bool level_changed = updated.level != entry.record.level;
  if (before_progression_notification_for_test_) {
    before_progression_notification_for_test_();
  }
  GuildExperienceChanged notification{updated.id, updated.experience, updated.level,
                                       level_changed, recipients(entry, 0)};
  events.reserve(events.size() + 1);
  try {
    uncertain_progression_ = UncertainProgression{entry.record.id, std::nullopt};
    progression_readback_complete_ = false;
    store_->update_guild(updated);
  } catch (const std::exception &error) {
    spdlog::error("Guild {} progression was not stored: {}", updated.id, error.what());
    return false;
  }

  entry.record = updated;
  uncertain_progression_.reset();
  events.push_back(std::move(notification));
  if (level_changed) {
    spdlog::info("Guild {} reached level {}", updated.id, updated.level);
  }
  return true;
}

GuildListResult GuildService::joinable_locked(const GuildActor &actor, int start_index) const {
  GuildListResult result;
  result.error = validate_actor(actor);
  if (result.error != x5::ec::Success) {
    return result;
  }
  if (start_index < 0) {
    result.error = x5::ec::Guild_InvalidReqParam;
    return result;
  }

  std::vector<const GuildEntry *> candidates;
  for (const auto &[guild_id, entry] : guilds_) {
    const bool open = entry.record.join_type == GuildJoinType::automatic;
    if (open && entry.members.size() < member_limit(entry.record)) {
      candidates.push_back(&entry);
    }
  }

  const auto first = static_cast<std::size_t>(start_index);
  if (first >= candidates.size()) {
    return result;
  }

  const auto last = std::min(candidates.size(), first + rules_.joinable_list_page_size);
  for (auto index = first; index < last; ++index) {
    result.guilds.push_back(summarize(*candidates[index]));
  }
  if (last < candidates.size()) {
    result.next_start_index = static_cast<int>(last);
  }
  return result;
}

GuildSearchResult GuildService::search_locked(const GuildActor &actor,
                                              const std::string &name) const {
  GuildSearchResult result;
  result.error = validate_actor(actor);
  if (result.error != x5::ec::Success) {
    return result;
  }
  if (name.empty() || name.size() > kMaximumSearchNameBytes) {
    result.error = x5::ec::Guild_InvalidReqParam;
    return result;
  }

  const auto found = guild_by_folded_name_.find(ascii_lower(name));
  if (found == guild_by_folded_name_.end()) {
    result.error = x5::ec::Guild_NotExist;
    return result;
  }

  result.guild = summarize(guilds_.at(found->second));
  return result;
}

GuildDetailResult GuildService::detail_locked(const GuildActor &actor,
                                              std::int64_t guild_id) const {
  GuildDetailResult result;
  result.error = validate_actor(actor);
  if (result.error != x5::ec::Success) {
    return result;
  }

  const auto *entry = find_guild(guild_id);
  if (!entry) {
    result.error = x5::ec::Guild_NotExist;
    return result;
  }

  result.guild = entry->record;
  result.members = roster(*entry);
  return result;
}

GuildCreateResult GuildService::create_locked(const GuildActor &actor,
                                              const GuildCreateRequest &request,
                                              std::vector<GuildEvent> &events) {
  GuildCreateResult result;
  result.error = validate_actor(actor);
  if (result.error != x5::ec::Success) {
    return result;
  }

  const auto now = clock_.epoch_ticks();
  if (guild_of(actor.char_id)) {
    result.error = x5::ec::Guild_AlreadyJoined;
    return result;
  }
  if (cooldown_active(history_of(actor.char_id).last_disband_epoch_ticks,
                      rules_.recreate_cooldown, now)) {
    result.error = x5::ec::Guild_InvalidTimeCreate;
    return result;
  }

  const auto name_length = utf8_code_points(request.name);
  if (!name_length || *name_length < rules_.name_min_length ||
      *name_length > rules_.name_max_length) {
    result.error = x5::ec::Guild_CreateNameLengthInvalid;
    return result;
  }
  if (!valid_guild_name_text(request.name)) {
    result.error = x5::ec::Guild_CreateNameInvalid;
    return result;
  }
  if (guild_by_folded_name_.contains(ascii_lower(request.name))) {
    result.error = x5::ec::Guild_CreateNameAlreadyUsed;
    return result;
  }

  const auto description_length = utf8_code_points(request.description);
  if (!description_length || *description_length > rules_.description_max_length) {
    result.error = x5::ec::Guild_CreateDescLengthInvalid;
    return result;
  }
  if (!valid_guild_description_text(request.description)) {
    result.error = x5::ec::Guild_CreateDescInvalid;
    return result;
  }

  result.error = validate_join_settings(request.join_type, request.join_level,
                                        request.join_company_position);
  if (result.error != x5::ec::Success) {
    return result;
  }

  // The client's GuildCreate dialogue refuses a character without a company (1106) or below
  // GuildCreatableCompanyPosition (1107); the server applies the same rule.
  if (rules_.companies_enabled && actor.company_type == 0) {
    result.error = x5::ec::Guild_InvalidCharCompay;
    return result;
  }
  if (rules_.companies_enabled &&
      actor.company_position < rules_.creatable_company_position) {
    result.error = x5::ec::Guild_InvalidCompanyPosition;
    return result;
  }

  GuildRecord guild;
  guild.world_id = world_id_;
  guild.name = request.name;
  guild.description = request.description;
  guild.join_type = static_cast<GuildJoinType>(request.join_type);
  guild.join_level = request.join_level;
  guild.join_company_type = actor.company_type;
  guild.join_company_position = request.join_company_position;
  guild.level = rules_.levels.front().level;
  guild.created_epoch_ticks = now;

  GuildMemberRecord master;
  master.char_id = actor.char_id;
  master.grade = GuildGrade::master;
  master.name = actor.name;
  master.level = actor.level;
  master.char_class = actor.char_class;
  master.company_position = actor.company_position;
  master.account_id = actor.account_id;
  master.joined_epoch_ticks = now;
  master.last_connect_epoch_ticks = now;

  std::optional<GuildRecord> stored;
  try {
    stored = store_->create_guild(guild, master);
  } catch (const std::exception &error) {
    spdlog::error("Guild create failed for character {}: {}", actor.char_id, error.what());
    result.error = x5::ec::Guild_FailedCreate;
    return result;
  }
  if (!stored) {
    result.error = x5::ec::Guild_CreateNameAlreadyUsed;
    return result;
  }

  master.guild_id = stored->id;
  auto &entry = guilds_[stored->id];
  entry.record = *stored;
  entry.members.emplace(master.char_id, master);
  guild_by_member_[master.char_id] = stored->id;
  guild_by_folded_name_[ascii_lower(stored->name)] = stored->id;
  invites_.erase(actor.char_id);

  events.push_back(GuildMemberJoined{entry.record, master, {master}, {}, false});
  spdlog::info("Guild {} created by character {}", stored->id, actor.char_id);

  result.guild = entry.record;
  result.owner = master;
  return result;
}

GuildManageResult GuildService::manage_locked(const GuildActor &actor,
                                              const GuildManageRequest &request,
                                              std::vector<GuildEvent> &events) {
  GuildManageResult result;
  result.error = validate_actor(actor);
  if (result.error != x5::ec::Success) {
    return result;
  }

  auto *entry = find_guild(request.guild_id);
  if (!entry) {
    result.error = x5::ec::Guild_NotExist;
    return result;
  }
  const auto member = entry->members.find(actor.char_id);
  if (member == entry->members.end()) {
    result.error = x5::ec::Guild_NotMember;
    return result;
  }
  if (!is_manager(member->second.grade)) {
    result.error = x5::ec::Guild_NoPermission;
    return result;
  }

  const auto description_length = utf8_code_points(request.description);
  if (!description_length || *description_length > rules_.description_max_length) {
    result.error = x5::ec::Guild_CreateDescLengthInvalid;
    return result;
  }
  if (!valid_guild_description_text(request.description)) {
    result.error = x5::ec::Guild_CreateDescInvalid;
    return result;
  }

  // Candidate: no separate notice limit is exported, so the description rules apply.
  const auto notice_length = utf8_code_points(request.notice);
  if (!notice_length || *notice_length > rules_.description_max_length) {
    result.error = x5::ec::Guild_CreateNoticeLengthInvalid;
    return result;
  }
  if (!valid_guild_description_text(request.notice)) {
    result.error = x5::ec::Guild_CreateNoticeInvalid;
    return result;
  }

  result.error = validate_join_settings(request.join_type, request.join_level,
                                        request.join_company_position);
  if (result.error != x5::ec::Success) {
    return result;
  }

  auto updated = entry->record;
  updated.description = request.description;
  updated.join_type = static_cast<GuildJoinType>(request.join_type);
  updated.join_level = request.join_level;
  updated.join_company_position = request.join_company_position;
  if (updated.notice != request.notice) {
    updated.notice = request.notice;
    updated.notice_char_id = actor.char_id;
    updated.notice_epoch_ticks = clock_.epoch_ticks();
  }

  auto result_guild = updated;
  GuildInfoChanged notification{updated, recipients(*entry, actor.char_id)};
  events.reserve(events.size() + 1);
  uncertain_progression_ = UncertainProgression{entry->record.id, std::nullopt, true};
  progression_readback_complete_ = false;
  try {
    store_->update_guild(updated);
  } catch (const std::exception &error) {
    spdlog::error("Guild {} update failed: {}", updated.id, error.what());
    result.error = x5::ec::Guild_FailedUpdate;
    return result;
  }

  using std::swap;
  swap(entry->record, updated);
  uncertain_progression_.reset();
  result.guild = std::move(result_guild);
  events.push_back(std::move(notification));
  return result;
}

GuildMemberManageResult GuildService::manage_member_locked(const GuildActor &actor,
                                                           std::int64_t guild_id,
                                                           std::int64_t target_char_id,
                                                           int grade,
                                                           std::vector<GuildEvent> &events) {
  GuildMemberManageResult result;
  result.char_id = target_char_id;
  result.error = validate_actor(actor);
  if (result.error != x5::ec::Success) {
    return result;
  }

  auto *entry = find_guild(guild_id);
  if (!entry) {
    result.error = x5::ec::Guild_NotExist;
    return result;
  }
  const auto actor_member = entry->members.find(actor.char_id);
  if (actor_member == entry->members.end()) {
    result.error = x5::ec::Guild_NotMember;
    return result;
  }
  if (grade < static_cast<int>(GuildGrade::none) || grade > static_cast<int>(GuildGrade::member)) {
    result.error = x5::ec::Guild_InvalidGrade;
    return result;
  }
  if (target_char_id == actor.char_id) {
    result.error = x5::ec::Guild_InvalidReqParam;
    return result;
  }

  const auto target = entry->members.find(target_char_id);
  if (target == entry->members.end()) {
    result.error = x5::ec::Guild_NotExistSelectedGuildMember;
    return result;
  }
  if (target->second.grade == GuildGrade::master) {
    result.error = x5::ec::Guild_InvalidGrade;
    return result;
  }

  // Copies: kick and change_grade modify the member map these iterators point into.
  const auto actor_record = actor_member->second;
  const auto target_record = target->second;
  const auto requested = static_cast<GuildGrade>(grade);
  if (requested == GuildGrade::none) {
    result.error = kick(*entry, actor_record, target_record, events);
  } else {
    result.error = change_grade(*entry, actor_record, target_record, requested, events);
  }

  result.grade = requested;
  return result;
}

GuildJoinResult GuildService::join_locked(const GuildActor &actor, std::int64_t guild_id,
                                          std::vector<GuildEvent> &events) {
  GuildJoinResult result;
  result.error = validate_actor(actor);
  if (result.error != x5::ec::Success) {
    return result;
  }
  if (guild_of(actor.char_id)) {
    result.error = x5::ec::Guild_AlreadyJoined;
    return result;
  }

  auto *entry = find_guild(guild_id);
  if (!entry) {
    result.error = x5::ec::Guild_NotExist;
    return result;
  }
  if (entry->record.join_type != GuildJoinType::automatic) {
    result.error = x5::ec::Guild_NotJoinableGuild;
    return result;
  }

  result.error = admit(actor, *entry, true, clock_.epoch_ticks());
  if (result.error != x5::ec::Success) {
    return result;
  }

  result.error = add_member(*entry, actor, false, events, &result);
  return result;
}

GuildLeaveResult GuildService::leave_locked(const GuildActor &actor, std::int64_t guild_id,
                                            std::vector<GuildEvent> &events) {
  GuildLeaveResult result;
  result.error = validate_actor(actor);
  if (result.error != x5::ec::Success) {
    return result;
  }

  auto *entry = find_guild(guild_id);
  if (!entry || !entry->members.contains(actor.char_id)) {
    result.error = x5::ec::Guild_NotMember;
    return result;
  }
  const auto member = entry->members.find(actor.char_id);

  // Candidate: the master transfers mastership or disbands instead of leaving.
  if (member->second.grade == GuildGrade::master) {
    result.error = x5::ec::Guild_InvalidGrade;
    return result;
  }

  const auto now = clock_.epoch_ticks();
  auto history = history_of(actor.char_id);
  history.last_leave_epoch_ticks = now;
  try {
    store_->remove_member(guild_id, actor.char_id, history);
  } catch (const std::exception &error) {
    spdlog::error("Guild {} leave failed for character {}: {}", guild_id, actor.char_id,
                  error.what());
    result.error = x5::ec::Guild_FailedMemberUpdate;
    return result;
  }

  entry->members.erase(member);
  guild_by_member_.erase(actor.char_id);
  histories_[actor.char_id] = history;
  events.push_back(
      GuildMemberLeft{guild_id, actor.char_id, false, now, recipients(*entry, actor.char_id)});

  result.left_epoch_ticks = now;
  return result;
}

GuildDisbandResult GuildService::disband_locked(const GuildActor &actor, std::int64_t guild_id,
                                                std::vector<GuildEvent> &events) {
  GuildDisbandResult result;
  result.error = validate_actor(actor);
  if (result.error != x5::ec::Success) {
    return result;
  }

  auto *entry = find_guild(guild_id);
  if (!entry) {
    result.error = x5::ec::Guild_NotExist;
    return result;
  }
  const auto member = entry->members.find(actor.char_id);
  if (member == entry->members.end()) {
    result.error = x5::ec::Guild_NotMember;
    return result;
  }
  if (member->second.grade != GuildGrade::master) {
    result.error = x5::ec::Guild_NoPermission;
    return result;
  }

  const auto now = clock_.epoch_ticks();
  if (cooldown_active(entry->record.created_epoch_ticks, rules_.disband_cooldown, now)) {
    result.error = x5::ec::Guild_DisbandCooltime;
    return result;
  }

  // Only the master's recreate cooldown starts; disbanding does not penalize members as a
  // voluntary leave would (Candidate).
  auto master_history = history_of(actor.char_id);
  master_history.last_disband_epoch_ticks = now;
  try {
    store_->delete_guild(guild_id, {master_history});
  } catch (const std::exception &error) {
    spdlog::error("Guild {} disband failed: {}", guild_id, error.what());
    result.error = x5::ec::Guild_FailedUpdate;
    return result;
  }

  remove_guild_locked(*entry, &master_history, events);
  spdlog::info("Guild {} disbanded by character {}", guild_id, actor.char_id);

  result.disbanded_epoch_ticks = now;
  return result;
}

void GuildService::remove_guild_locked(GuildEntry &entry, const GuildHistory *master_history,
                                       std::vector<GuildEvent> &events) {
  const auto guild_id = entry.record.id;
  std::vector<std::int64_t> former_members;
  for (const auto &[member_id, record] : entry.members) {
    former_members.push_back(member_id);
    guild_by_member_.erase(member_id);
    experience_remainders_.erase(member_id);
  }

  // The acting master already has its response; a GM disband notifies every member.
  const auto excluded = master_history ? master_history->char_id : 0;
  auto event_recipients = recipients(entry, excluded);
  if (master_history) {
    histories_[master_history->char_id] = *master_history;
  }
  guild_by_folded_name_.erase(ascii_lower(entry.record.name));
  forget_invites_for_guild(guild_id);
  guilds_.erase(guild_id);

  events.push_back(
      GuildDisbanded{guild_id, std::move(former_members), std::move(event_recipients)});
}

GuildInviteResult GuildService::invite_locked(const GuildActor &inviter, std::int64_t guild_id,
                                              const GuildActor &target,
                                              std::vector<GuildEvent> &events) {
  GuildInviteResult result;
  result.target_name = target.name;
  result.error = validate_actor(inviter);
  if (result.error != x5::ec::Success) {
    return result;
  }
  if (validate_actor(target) != x5::ec::Success) {
    result.error = x5::ec::Guild_NotExistsInvitee;
    return result;
  }

  const auto *entry = find_guild(guild_id);
  if (!entry) {
    result.error = x5::ec::Guild_NotExistGuild;
    return result;
  }
  const auto member = entry->members.find(inviter.char_id);
  if (member == entry->members.end()) {
    result.error = x5::ec::Guild_NotMember;
    return result;
  }
  if (!is_manager(member->second.grade)) {
    result.error = x5::ec::Guild_NotLeaderOrSubLeader;
    return result;
  }
  if (target.char_id == inviter.char_id) {
    result.error = x5::ec::Guild_InvalidReqParam;
    return result;
  }

  const auto *target_guild = guild_of(target.char_id);
  if (target_guild) {
    result.error = target_guild == entry ? x5::ec::Guild_AlreadyGuildJoin
                                         : x5::ec::Guild_InAnotherGuild;
    return result;
  }

  prune_expired_invites();
  if (invites_.contains(target.char_id)) {
    result.error = x5::ec::Guild_AlreadyInvited;
    return result;
  }
  if (entry->members.size() >= member_limit(entry->record)) {
    result.error = x5::ec::Guild_FullCapacity;
    return result;
  }

  const auto now = clock_.epoch_ticks();
  if (cooldown_active(history_of(target.char_id).last_leave_epoch_ticks, rules_.rejoin_cooldown,
                      now)) {
    result.error = x5::ec::Guild_InvalidTimeJoin;
    return result;
  }
  if (invites_.size() >= kMaximumPendingInvites) {
    result.error = x5::ec::Guild_LimitRequestCount;
    return result;
  }

  const auto expires_at = clock_.steady() + rules_.invite_timeout;
  invites_[target.char_id] = PendingInvite{guild_id, inviter.char_id, expires_at};
  events.push_back(
      GuildInviteIssued{guild_id, entry->record.name, inviter.char_id, target.char_id});
  return result;
}

EErrorCode GuildService::accept_invite_locked(const GuildActor &invitee, std::int64_t guild_id,
                                              std::int64_t inviter_char_id,
                                              std::vector<GuildEvent> &events) {
  const auto actor_error = validate_actor(invitee);
  if (actor_error != x5::ec::Success) {
    return actor_error;
  }

  prune_expired_invites();
  const auto pending = invites_.find(invitee.char_id);
  if (pending == invites_.end() || pending->second.guild_id != guild_id ||
      pending->second.inviter_char_id != inviter_char_id) {
    return x5::ec::Guild_NotExistsInviter;
  }
  invites_.erase(pending);

  if (guild_of(invitee.char_id)) {
    return x5::ec::Guild_AlreadyJoined;
  }
  auto *entry = find_guild(guild_id);
  if (!entry) {
    return x5::ec::Guild_NotExistGuild;
  }

  // The invitation already passed the manager check; the join level applies to self-joins.
  const auto admission = admit(invitee, *entry, false, clock_.epoch_ticks());
  if (admission != x5::ec::Success) {
    return admission;
  }

  return add_member(*entry, invitee, true, events, nullptr);
}

EErrorCode GuildService::reject_invite_locked(const GuildActor &invitee, std::int64_t guild_id,
                                              std::int64_t inviter_char_id,
                                              std::vector<GuildEvent> &events) {
  const auto actor_error = validate_actor(invitee);
  if (actor_error != x5::ec::Success) {
    return actor_error;
  }

  prune_expired_invites();
  const auto pending = invites_.find(invitee.char_id);
  if (pending == invites_.end() || pending->second.guild_id != guild_id ||
      pending->second.inviter_char_id != inviter_char_id) {
    return x5::ec::Guild_NotExistsInviter;
  }
  invites_.erase(pending);

  events.push_back(GuildInviteRejected{inviter_char_id, invitee.name,
                                       GuildInviteRejectReason::user_rejected});
  return x5::ec::Success;
}

void GuildService::refresh_member_locked(const GuildActor &actor,
                                         std::vector<GuildEvent> &events) {
  if (validate_actor(actor) != x5::ec::Success) {
    return;
  }

  const auto guild_id = guild_by_member_.find(actor.char_id);
  if (guild_id == guild_by_member_.end()) {
    return;
  }
  auto &entry = guilds_.at(guild_id->second);
  auto &member = entry.members.at(actor.char_id);

  auto updated = member;
  updated.name = actor.name;
  updated.level = actor.level;
  updated.char_class = actor.char_class;
  updated.company_position = actor.company_position;
  if (actor.account_id != 0) {
    updated.account_id = actor.account_id;
  }
  updated.last_connect_epoch_ticks = clock_.epoch_ticks();
  try {
    store_->save_members({updated});
  } catch (const std::exception &error) {
    spdlog::warn("Guild member {} refresh was not stored: {}", actor.char_id, error.what());
    return;
  }

  const bool display_changed = updated.name != member.name || updated.level != member.level ||
                               updated.char_class != member.char_class ||
                               updated.company_position != member.company_position;
  member = updated;
  if (display_changed) {
    events.push_back(GuildMemberUpdated{updated, recipients(entry, actor.char_id)});
  }
}

void GuildService::load_snapshot(GuildSnapshot snapshot) {
  for (auto &guild : snapshot.guilds) {
    if (guild.world_id != world_id_) {
      continue;
    }
    guild_by_folded_name_[ascii_lower(guild.name)] = guild.id;
    guilds_[guild.id].record = std::move(guild);
  }

  for (auto &member : snapshot.members) {
    auto *entry = find_guild(member.guild_id);
    if (!entry) {
      continue;
    }
    guild_by_member_[member.char_id] = member.guild_id;
    entry->members[member.char_id] = std::move(member);
  }

  for (const auto &history : snapshot.histories) {
    histories_[history.char_id] = history;
  }
}

void GuildService::repair_loaded_guilds() {
  std::vector<std::int64_t> empty_guilds;
  for (auto &[guild_id, entry] : guilds_) {
    if (entry.members.empty()) {
      empty_guilds.push_back(guild_id);
      continue;
    }

    const bool has_master = std::ranges::any_of(entry.members, [](const auto &member) {
      return member.second.grade == GuildGrade::master;
    });
    if (has_master) {
      continue;
    }

    // Deleting a master character cascades its membership row away.
    auto successor = roster(entry).front();
    successor.grade = GuildGrade::master;
    store_->save_members({successor});
    entry.members[successor.char_id] = successor;
    spdlog::warn("Guild {} had no master; promoted character {}", guild_id, successor.char_id);
  }

  for (const auto guild_id : empty_guilds) {
    store_->delete_guild(guild_id, {});
    guild_by_folded_name_.erase(ascii_lower(guilds_.at(guild_id).record.name));
    guilds_.erase(guild_id);
    spdlog::warn("Guild {} had no members and was deleted", guild_id);
  }
}

const GuildService::GuildEntry *GuildService::find_guild(std::int64_t guild_id) const {
  const auto found = guilds_.find(guild_id);
  return found == guilds_.end() ? nullptr : &found->second;
}

GuildService::GuildEntry *GuildService::find_guild(std::int64_t guild_id) {
  const auto found = guilds_.find(guild_id);
  return found == guilds_.end() ? nullptr : &found->second;
}

const GuildService::GuildEntry *GuildService::guild_of(std::int64_t char_id) const {
  const auto found = guild_by_member_.find(char_id);
  if (found == guild_by_member_.end()) {
    return nullptr;
  }
  return find_guild(found->second);
}

GuildHistory GuildService::history_of(std::int64_t char_id) const {
  const auto found = histories_.find(char_id);
  if (found == histories_.end()) {
    GuildHistory history;
    history.char_id = char_id;
    return history;
  }
  return found->second;
}

bool GuildService::cooldown_active(std::int64_t anchor_ticks, std::chrono::hours cooldown,
                                   std::int64_t now_ticks) const {
  if (anchor_ticks <= 0 || cooldown.count() <= 0) {
    return false;
  }

  const auto cooldown_ticks = cooldown.count() * kTicksPerHour;
  if (anchor_ticks > std::numeric_limits<std::int64_t>::max() - cooldown_ticks) {
    return true;
  }
  return now_ticks < anchor_ticks + cooldown_ticks;
}

std::size_t GuildService::member_limit(const GuildRecord &guild) const {
  for (const auto &rule : rules_.levels) {
    if (rule.level == guild.level) {
      return rule.max_members;
    }
  }
  return rules_.levels.front().max_members;
}

std::size_t GuildService::sub_master_count(const GuildEntry &entry) const {
  return static_cast<std::size_t>(std::ranges::count_if(entry.members, [](const auto &member) {
    return member.second.grade == GuildGrade::sub_master;
  }));
}

GuildSummary GuildService::summarize(const GuildEntry &entry) const {
  GuildSummary summary;
  summary.guild = entry.record;
  summary.member_count = entry.members.size();
  for (const auto &[member_id, member] : entry.members) {
    if (member.grade == GuildGrade::master) {
      summary.master_name = member.name;
    }
  }
  return summary;
}

std::vector<GuildMemberRecord> GuildService::roster(const GuildEntry &entry) const {
  std::vector<GuildMemberRecord> members;
  members.reserve(entry.members.size());
  for (const auto &[member_id, member] : entry.members) {
    members.push_back(member);
  }
  std::ranges::sort(members, roster_order);
  return members;
}

std::vector<std::int64_t> GuildService::recipients(const GuildEntry &entry,
                                                   std::int64_t excluded) const {
  std::vector<std::int64_t> result;
  for (const auto &[member_id, member] : entry.members) {
    if (member_id != excluded) {
      result.push_back(member_id);
    }
  }
  return result;
}

EErrorCode GuildService::validate_actor(const GuildActor &actor) const {
  if (actor.world_id != world_id_ || actor.char_id <= 0) {
    return x5::ec::Guild_InvalidChar;
  }
  return x5::ec::Success;
}

EErrorCode GuildService::validate_join_settings(int join_type, int join_level,
                                                int join_company_position) const {
  const bool known_type = join_type == static_cast<int>(GuildJoinType::none) ||
                          join_type == static_cast<int>(GuildJoinType::automatic);
  const bool level_in_range = join_level >= 0 && join_level <= kMaximumJoinLevel;
  const bool position_in_range =
      join_company_position >= 0 && join_company_position <= kMaximumJoinCompanyPosition;
  if (!known_type || !level_in_range || !position_in_range) {
    return x5::ec::Guild_InvalidReqParam;
  }
  return x5::ec::Success;
}

EErrorCode GuildService::admit(const GuildActor &actor, const GuildEntry &entry,
                               bool check_join_level, std::int64_t now_ticks) const {
  if (cooldown_active(history_of(actor.char_id).last_leave_epoch_ticks, rules_.rejoin_cooldown,
                      now_ticks)) {
    return x5::ec::Guild_InvalidTimeJoin;
  }
  if (check_join_level && actor.level < entry.record.join_level) {
    return x5::ec::Guild_InvalidCharLevel;
  }

  const auto company_error = check_company(actor, entry);
  if (company_error != x5::ec::Success) {
    return company_error;
  }

  if (entry.members.size() >= member_limit(entry.record)) {
    return x5::ec::Guild_MemberLimitExceeded;
  }
  return x5::ec::Success;
}

EErrorCode GuildService::check_company(const GuildActor &actor, const GuildEntry &entry) const {
  if (!rules_.companies_enabled) {
    return x5::ec::Success;
  }

  // Candidate: a guild accepts only members of its founder's company. Guilds stored before
  // companies existed have join_company_type 0 and accept any company.
  const auto guild_company = entry.record.join_company_type;
  if (guild_company != 0 && actor.company_type != guild_company) {
    return x5::ec::Guild_InvalidCompanyType;
  }
  // The client refuses a join below join_company_position (DialogueErrorCode-free check in
  // AAC_Guild_UI_RequestJoinGuild, error 23035).
  if (actor.company_position < entry.record.join_company_position) {
    return x5::ec::Guild_InvalidCompanyPosition;
  }
  return x5::ec::Success;
}

EErrorCode GuildService::add_member(GuildEntry &entry, const GuildActor &actor,
                                    bool via_invitation, std::vector<GuildEvent> &events,
                                    GuildJoinResult *result) {
  const auto now = clock_.epoch_ticks();
  GuildMemberRecord member;
  member.guild_id = entry.record.id;
  member.char_id = actor.char_id;
  member.grade = GuildGrade::member;
  member.name = actor.name;
  member.level = actor.level;
  member.char_class = actor.char_class;
  member.company_position = actor.company_position;
  member.account_id = actor.account_id;
  member.joined_epoch_ticks = now;
  member.last_connect_epoch_ticks = now;

  try {
    store_->save_members({member});
  } catch (const std::exception &error) {
    spdlog::error("Guild {} join failed for character {}: {}", entry.record.id, actor.char_id,
                  error.what());
    return x5::ec::Guild_FailedMemberUpdate;
  }

  entry.members[member.char_id] = member;
  guild_by_member_[member.char_id] = entry.record.id;
  invites_.erase(member.char_id);

  auto members = roster(entry);
  events.push_back(GuildMemberJoined{entry.record, member, members,
                                     recipients(entry, actor.char_id), via_invitation});
  if (result) {
    result->guild = entry.record;
    result->member = member;
    result->members = std::move(members);
  }
  return x5::ec::Success;
}

EErrorCode GuildService::kick(GuildEntry &entry, const GuildMemberRecord &actor_member,
                              const GuildMemberRecord &target, std::vector<GuildEvent> &events) {
  // Client rule: a Master or SubMaster may remove only a plain Member.
  if (!is_manager(actor_member.grade) || target.grade != GuildGrade::member) {
    return x5::ec::Guild_NoPermission;
  }

  const auto now = clock_.epoch_ticks();
  auto history = history_of(target.char_id);
  history.last_leave_epoch_ticks = now;
  try {
    store_->remove_member(entry.record.id, target.char_id, history);
  } catch (const std::exception &error) {
    spdlog::error("Guild {} kick failed for character {}: {}", entry.record.id, target.char_id,
                  error.what());
    return x5::ec::Guild_FailedMemberUpdate;
  }

  entry.members.erase(target.char_id);
  guild_by_member_.erase(target.char_id);
  histories_[target.char_id] = history;
  events.push_back(GuildMemberLeft{entry.record.id, target.char_id, true, now,
                                   recipients(entry, actor_member.char_id)});
  return x5::ec::Success;
}

EErrorCode GuildService::change_grade(GuildEntry &entry, const GuildMemberRecord &actor_member,
                                      const GuildMemberRecord &target, GuildGrade grade,
                                      std::vector<GuildEvent> &events) {
  // Client rule: only the Master changes grades.
  if (actor_member.grade != GuildGrade::master) {
    return x5::ec::Guild_NoPermission;
  }
  if (target.grade == grade) {
    return x5::ec::Guild_MemberGradeAlreadyChanged;
  }
  if (grade == GuildGrade::sub_master && sub_master_count(entry) >= rules_.max_sub_masters) {
    return x5::ec::Guild_ReachedSubmasterLimit;
  }

  auto promoted = target;
  promoted.grade = grade;
  std::vector<GuildMemberRecord> changed;
  if (grade == GuildGrade::master) {
    auto demoted = actor_member;
    demoted.grade = GuildGrade::member;
    changed.push_back(demoted);
  }
  changed.push_back(promoted);

  try {
    store_->save_members(changed);
  } catch (const std::exception &error) {
    spdlog::error("Guild {} grade change failed for character {}: {}", entry.record.id,
                  target.char_id, error.what());
    return x5::ec::Guild_FailedMemberUpdate;
  }

  for (const auto &member : changed) {
    entry.members[member.char_id] = member;
  }

  events.push_back(GuildMemberGradeChanged{entry.record.id, promoted.char_id, promoted.grade,
                                           recipients(entry, actor_member.char_id)});
  if (grade == GuildGrade::master) {
    // The former master's response names only the target, so it is told its own demotion.
    events.push_back(GuildMemberGradeChanged{entry.record.id, actor_member.char_id,
                                             GuildGrade::member, recipients(entry, 0)});
  }
  return x5::ec::Success;
}

void GuildService::prune_expired_invites() {
  const auto now = clock_.steady();
  std::erase_if(invites_, [now](const auto &entry) { return entry.second.expires_at <= now; });
}

void GuildService::forget_invites_for_guild(std::int64_t guild_id) {
  std::erase_if(invites_,
                [guild_id](const auto &entry) { return entry.second.guild_id == guild_id; });
}

void GuildService::publish(const std::vector<GuildEvent> &events, bool require_delivery) const {
  if (events.empty()) {
    return;
  }

  std::vector<GuildEventSubscriber> subscribers;
  {
    std::lock_guard lock(mutex_);
    subscribers = subscribers_;
  }

  std::exception_ptr delivery_failure;
  for (const auto &subscriber : subscribers) {
    try {
      subscriber(events);
    } catch (const std::exception &error) {
      // One transport failing must not hide the committed change from the others.
      spdlog::error("Guild event delivery failed: {}", error.what());
      if (!delivery_failure) {
        delivery_failure = std::current_exception();
      }
    } catch (...) {
      spdlog::error("Guild event delivery failed with a non-standard exception");
      if (!delivery_failure) {
        delivery_failure = std::current_exception();
      }
    }
  }
  if (require_delivery && delivery_failure) {
    std::rethrow_exception(delivery_failure);
  }
}

} // namespace chronicles
