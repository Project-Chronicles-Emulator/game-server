#include "world/features/guild/guild.hpp"

#include "persistence/account_repository.hpp"
#include "persistence/character_repository.hpp"
#include "persistence/memory_quest_service_coordinator.hpp"
#include "persistence/quest_service_effect_codec.hpp"
#include "persistence/quest_service_effect_guards.hpp"

#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace chronicles {
namespace {

GuildJoinType stored_join_type(int value) {
  if (value != static_cast<int>(GuildJoinType::none) &&
      value != static_cast<int>(GuildJoinType::automatic)) {
    throw std::runtime_error("Guild has an unknown stored join type");
  }
  return static_cast<GuildJoinType>(value);
}

GuildGrade stored_grade(int value) {
  if (value < static_cast<int>(GuildGrade::master) ||
      value > static_cast<int>(GuildGrade::member)) {
    throw std::runtime_error("Guild member has an unknown stored grade");
  }
  return static_cast<GuildGrade>(value);
}

void validate_stored_member(const GuildMemberRecord &member) {
  if (member.guild_id <= 0 || member.char_id <= 0 || member.grade == GuildGrade::none) {
    throw std::invalid_argument("Guild membership requires a guild, a character and a grade");
  }
}

void require_character_guild_write_allowed(pqxx::work &transaction,
                                           std::int64_t character_id) {
  const auto pending = transaction.exec(
      "SELECT 1 FROM emulator_quest_service_effects "
      "WHERE character_id=$1 AND NOT guild_applied LIMIT 1",
      pqxx::params{character_id});
  if (!pending.empty()) {
    throw std::runtime_error("Guild membership update waits for a pending quest reward");
  }
}

void require_memory_owner_epoch(const MemoryQuestServiceCoordinator &coordinator,
                                std::int64_t adopted_epoch) {
  if (adopted_epoch <= 0 || coordinator.owner_epoch != adopted_epoch) {
    throw std::runtime_error("Guild store owner epoch is stale or missing");
  }
}

bool same_effect_identity(const QuestServiceEffectEvent &left,
                          const QuestServiceEffectEvent &right) {
  return left.enqueue_sequence == right.enqueue_sequence &&
         left.account_id == right.account_id && left.character_id == right.character_id &&
         left.world_id == right.world_id && left.effect_id == right.effect_id &&
         left.mutation_fingerprint == right.mutation_fingerprint &&
         left.monster_receipt == right.monster_receipt && left.effects == right.effects &&
         left.granted_items == right.granted_items;
}

void validate_progression_update(const GuildRecord &before_guild,
                                 const GuildMemberRecord &before_member,
                                 const GuildRecord &after_guild,
                                 const GuildMemberRecord &after_member) {
  if (before_guild.id <= 0 || before_member.char_id <= 0 ||
      before_member.guild_id != before_guild.id || after_guild.id != before_guild.id ||
      after_member.char_id != before_member.char_id ||
      after_member.guild_id != before_guild.id || after_guild.experience < 0 ||
      after_guild.level < 1 || after_member.contribution < before_member.contribution) {
    throw std::invalid_argument("Invalid guild progression update");
  }
}

bool valid_activity_remainders(const QuestServiceEffectEvent &event,
                               std::int64_t before, std::int64_t after) {
  if (event.effects.activity_experience == 0) {
    return before == 0 && after == 0;
  }
  const auto rate = event.effects.activity_guild_rate;
  if (event.effects.activity_experience < 0 || rate <= 0 || before < 0 || before >= rate ||
      after < 0 || after >= rate) {
    return false;
  }
  const auto incoming = event.effects.activity_experience % rate;
  const auto expected = before >= rate - incoming ? before - (rate - incoming)
                                                   : before + incoming;
  return after == expected;
}

void upsert_history(pqxx::work &transaction, const GuildHistory &history) {
  transaction.exec(
      "INSERT INTO emulator_character_guild_history"
      "(character_id,last_leave_epoch_ticks,last_disband_epoch_ticks) VALUES ($1,$2,$3) "
      "ON CONFLICT (character_id) DO UPDATE SET "
      "last_leave_epoch_ticks=EXCLUDED.last_leave_epoch_ticks,"
      "last_disband_epoch_ticks=EXCLUDED.last_disband_epoch_ticks",
      pqxx::params{history.char_id, history.last_leave_epoch_ticks,
                   history.last_disband_epoch_ticks});
}

} // namespace

MemoryGuildStore::MemoryGuildStore(std::shared_ptr<MemoryQuestServiceCoordinator> quest_services)
    : quest_services_(std::move(quest_services)) {
}

void MemoryGuildStore::adopt_quest_service_owner_epoch(std::int64_t epoch) {
  if (epoch <= 0) {
    throw std::invalid_argument("Invalid guild store owner epoch");
  }
  owner_epoch_.store(epoch);
}

void MemoryGuildStore::set_before_membership_publish_for_test(std::function<void()> hook) {
  std::lock_guard lock(mutex_);
  before_membership_publish_for_test_ = std::move(hook);
}

GuildQuestEffectResult MemoryGuildStore::quest_effect_locked(
    const QuestServiceEffectEvent &event) {
  const auto found = quest_services_->events.find({event.character_id, event.effect_id});
  if (found == quest_services_->events.end() || !same_effect_identity(found->second, event)) {
    return {QuestEffectSettlementStatus::identity_mismatch};
  }
  const auto guild = guilds_.find(event.effects.expected_guild_id);
  const auto member = members_.find(event.character_id);
  const auto membership = quest_services_->guild_memberships.find(event.character_id);
  const auto generation = quest_services_->guild_generations.find(event.character_id);
  if (guild == guilds_.end() || member == members_.end() ||
      guild->second.world_id != event.world_id || member->second.guild_id != guild->first ||
      membership == quest_services_->guild_memberships.end() || membership->second != guild->first ||
      generation == quest_services_->guild_generations.end() ||
      generation->second != event.effects.expected_guild_generation ||
      member->second.account_id != event.account_id) {
    return {QuestEffectSettlementStatus::identity_mismatch};
  }
  GuildQuestEffectResult result;
  result.guild = guild->second;
  result.member = member->second;
  result.event_completed = found->second.company_applied && found->second.guild_applied;
  if (event.effects.activity_experience > 0 && event.effects.activity_guild_rate > 0) {
    const auto key = std::make_tuple(event.world_id, event.character_id,
                                     QuestServiceComponent::guild,
                                     event.effects.expected_guild_generation,
                                     event.effects.activity_guild_rate);
    const auto remainder = quest_services_->activity_remainders.find(key);
    if (remainder != quest_services_->activity_remainders.end()) {
      result.activity_remainder = remainder->second;
    }
  }
  result.status = found->second.guild_applied ? QuestEffectSettlementStatus::already_applied
                                               : QuestEffectSettlementStatus::pending;
  if (!found->second.guild_applied &&
      !quest_services_->can_mark_component(found->second, QuestServiceComponent::guild)) {
    result.status = QuestEffectSettlementStatus::retry;
  }
  return result;
}

GuildQuestEffectResult MemoryGuildStore::read_quest_effect(const QuestServiceEffectEvent &event) {
  if (!quest_services_) {
    throw std::runtime_error("Guild quest service coordinator is missing");
  }
  std::lock_guard coordinator_lock(quest_services_->mutex);
  require_memory_owner_epoch(*quest_services_, owner_epoch_.load());
  std::lock_guard lock(mutex_);
  return quest_effect_locked(event);
}

GuildQuestEffectResult MemoryGuildStore::settle_quest_effect(
    const QuestServiceEffectEvent &event, const GuildRecord &before_guild,
    const GuildMemberRecord &before_member, const GuildRecord &after_guild,
    const GuildMemberRecord &after_member, std::int64_t before_remainder,
    std::int64_t after_remainder) {
  if (!quest_services_) {
    throw std::runtime_error("Guild quest service coordinator is missing");
  }
  std::lock_guard coordinator_lock(quest_services_->mutex);
  require_memory_owner_epoch(*quest_services_, owner_epoch_.load());
  std::lock_guard lock(mutex_);
  auto result = quest_effect_locked(event);
  if (result.status != QuestEffectSettlementStatus::pending) {
    return result;
  }
  if (before_guild.id != event.effects.expected_guild_id ||
      after_guild.id != before_guild.id || before_member.char_id != event.character_id ||
      after_member.char_id != before_member.char_id ||
      result.guild->experience != before_guild.experience ||
      result.guild->level != before_guild.level ||
      result.member->contribution != before_member.contribution ||
      result.activity_remainder != before_remainder ||
      !valid_activity_remainders(event, before_remainder, after_remainder) ||
      after_guild.experience < 0 || after_guild.level < 1 ||
      after_member.contribution < before_member.contribution) {
    result.status = QuestEffectSettlementStatus::retry;
    return result;
  }
  auto &guild = guilds_.at(before_guild.id);
  auto &member = members_.at(before_member.char_id);
  auto &stored_event = quest_services_->events.at({event.character_id, event.effect_id});
  if (!quest_services_->can_mark_component(stored_event, QuestServiceComponent::guild)) {
    result.status = QuestEffectSettlementStatus::retry;
    return result;
  }
  std::map<std::tuple<int, std::int64_t, QuestServiceComponent, std::int64_t, std::int64_t>,
           std::int64_t>::iterator remainder;
  if (event.effects.activity_experience > 0) {
    const auto key = std::make_tuple(event.world_id, event.character_id,
                                     QuestServiceComponent::guild,
                                     event.effects.expected_guild_generation,
                                     event.effects.activity_guild_rate);
    remainder = quest_services_->activity_remainders.try_emplace(key, before_remainder).first;
  }
  guild.experience = after_guild.experience;
  guild.level = after_guild.level;
  member.contribution = after_member.contribution;
  if (event.effects.activity_experience > 0) {
    remainder->second = after_remainder;
    result.activity_remainder = after_remainder;
  }
  quest_services_->mark_component_applied(stored_event, QuestServiceComponent::guild);
  result.event_completed = stored_event.company_applied && stored_event.guild_applied;
  result.guild->experience = guild.experience;
  result.guild->level = guild.level;
  result.member->contribution = member.contribution;
  result.status = QuestEffectSettlementStatus::applied;
  return result;
}

void MemoryGuildStore::commit_progression(const GuildRecord &before_guild,
                                          const GuildMemberRecord &before_member,
                                          const GuildRecord &after_guild,
                                          const GuildMemberRecord &after_member) {
  validate_progression_update(before_guild, before_member, after_guild, after_member);
  std::unique_lock<std::mutex> coordinator_lock;
  if (quest_services_) {
    coordinator_lock = std::unique_lock(quest_services_->mutex);
    require_memory_owner_epoch(*quest_services_, owner_epoch_.load());
    if (quest_services_->guild_effect_pending(before_member.char_id) ||
        quest_services_->guild_effect_pending_for_guild(before_guild.id)) {
      throw std::runtime_error("Guild progression waits for a pending quest reward");
    }
  }
  std::lock_guard lock(mutex_);
  const auto guild = guilds_.find(before_guild.id);
  const auto member = members_.find(before_member.char_id);
  if (guild == guilds_.end() || member == members_.end() ||
      member->second.guild_id != before_guild.id ||
      guild->second.experience != before_guild.experience ||
      guild->second.level != before_guild.level ||
      member->second.contribution != before_member.contribution) {
    throw std::runtime_error("Guild progression changed before commit");
  }
  guild->second.experience = after_guild.experience;
  guild->second.level = after_guild.level;
  member->second.contribution = after_member.contribution;
}

GuildProgressionSnapshot MemoryGuildStore::read_progression(
    int world_id, std::int64_t guild_id, const std::optional<std::int64_t> &character_id) {
  std::unique_lock<std::mutex> coordinator_lock;
  if (quest_services_) {
    coordinator_lock = std::unique_lock(quest_services_->mutex);
    require_memory_owner_epoch(*quest_services_, owner_epoch_.load());
  }
  std::lock_guard lock(mutex_);
  const auto guild = guilds_.find(guild_id);
  if (guild == guilds_.end() || guild->second.world_id != world_id) {
    throw std::runtime_error("Guild progression readback lost its guild");
  }
  GuildProgressionSnapshot result;
  result.guild = guild->second;
  if (character_id) {
    const auto member = members_.find(*character_id);
    if (member == members_.end() || member->second.guild_id != guild_id) {
      throw std::runtime_error("Guild progression readback lost its member");
    }
    result.member = member->second;
  }
  return result;
}

GuildSnapshot MemoryGuildStore::load(int world_id) {
  std::lock_guard lock(mutex_);
  GuildSnapshot snapshot;
  for (const auto &[guild_id, guild] : guilds_) {
    if (guild.world_id == world_id) {
      snapshot.guilds.push_back(guild);
    }
  }

  for (const auto &[char_id, member] : members_) {
    const auto guild = guilds_.find(member.guild_id);
    if (guild != guilds_.end() && guild->second.world_id == world_id) {
      snapshot.members.push_back(member);
    }
  }

  for (const auto &[char_id, history] : histories_) {
    snapshot.histories.push_back(history);
  }
  return snapshot;
}

std::optional<GuildRecord> MemoryGuildStore::create_guild(GuildRecord guild,
                                                          const GuildMemberRecord &master) {
  std::unique_lock<std::mutex> coordinator_lock;
  if (quest_services_) {
    coordinator_lock = std::unique_lock(quest_services_->mutex);
    require_memory_owner_epoch(*quest_services_, owner_epoch_.load());
    if (quest_services_->guild_effect_pending(master.char_id)) {
      throw std::runtime_error("Guild creation waits for a pending quest reward");
    }
  }
  std::lock_guard lock(mutex_);
  const auto folded_name = ascii_lower(guild.name);
  for (const auto &[guild_id, existing] : guilds_) {
    if (existing.world_id == guild.world_id && ascii_lower(existing.name) == folded_name) {
      return std::nullopt;
    }
  }
  if (members_.contains(master.char_id)) {
    throw std::runtime_error("Guild master already belongs to a guild");
  }

  auto memberships = quest_services_ ? quest_services_->guild_memberships
                                     : std::map<std::int64_t, std::int64_t>{};
  auto generations = quest_services_ ? quest_services_->guild_generations
                                     : std::map<std::int64_t, std::int64_t>{};
  if (next_guild_id_ == std::numeric_limits<std::int64_t>::max()) {
    throw std::overflow_error("Guild IDs are exhausted");
  }
  auto guilds = guilds_;
  auto members = members_;
  guild.id = next_guild_id_;
  if (quest_services_) {
    memberships[master.char_id] = guild.id;
    advance_quest_membership_generation(generations, master.char_id);
  }
  guilds[guild.id] = guild;

  auto stored_master = master;
  stored_master.guild_id = guild.id;
  members[stored_master.char_id] = stored_master;
  std::optional<GuildRecord> result(guild);
  if (before_membership_publish_for_test_) {
    before_membership_publish_for_test_();
  }
  guilds_.swap(guilds);
  members_.swap(members);
  if (quest_services_) {
    quest_services_->guild_memberships.swap(memberships);
    quest_services_->guild_generations.swap(generations);
  }
  ++next_guild_id_;
  return result;
}

void MemoryGuildStore::update_guild(const GuildRecord &guild) {
  std::unique_lock<std::mutex> coordinator_lock;
  if (quest_services_) {
    coordinator_lock = std::unique_lock(quest_services_->mutex);
    require_memory_owner_epoch(*quest_services_, owner_epoch_.load());
    if (quest_services_->guild_effect_pending_for_guild(guild.id)) {
      throw std::runtime_error("Guild update waits for a pending quest reward");
    }
  }
  std::lock_guard lock(mutex_);
  const auto found = guilds_.find(guild.id);
  if (found == guilds_.end()) {
    throw std::runtime_error("Guild to update does not exist");
  }
  auto updated = guild;
  using std::swap;
  swap(found->second, updated);
}

void MemoryGuildStore::save_members(const std::vector<GuildMemberRecord> &members) {
  std::unique_lock<std::mutex> coordinator_lock;
  if (quest_services_) {
    coordinator_lock = std::unique_lock(quest_services_->mutex);
    require_memory_owner_epoch(*quest_services_, owner_epoch_.load());
    for (const auto &member : members) {
      if (quest_services_->guild_effect_pending(member.char_id) ||
          quest_services_->guild_effect_pending_for_guild(member.guild_id)) {
        throw std::runtime_error("Guild membership waits for a pending quest reward");
      }
    }
  }
  std::lock_guard lock(mutex_);
  // Validate every row before changing any, so the batch applies atomically.
  std::map<std::int64_t, std::int64_t> batch_memberships;
  for (const auto &member : members) {
    validate_stored_member(member);
    if (!guilds_.contains(member.guild_id)) {
      throw std::runtime_error("Guild membership refers to a missing guild");
    }
    const auto existing = members_.find(member.char_id);
    if (existing != members_.end() && existing->second.guild_id != member.guild_id) {
      throw std::runtime_error("Character already belongs to another guild");
    }
    const auto [batch_entry, inserted] =
        batch_memberships.emplace(member.char_id, member.guild_id);
    if (!inserted && batch_entry->second != member.guild_id) {
      throw std::runtime_error("Guild membership batch moves a character between guilds");
    }
  }

  auto memberships = quest_services_ ? quest_services_->guild_memberships
                                     : std::map<std::int64_t, std::int64_t>{};
  auto generations = quest_services_ ? quest_services_->guild_generations
                                     : std::map<std::int64_t, std::int64_t>{};
  auto staged_members = members_;
  for (const auto &member : members) {
    if (quest_services_) {
      const auto previous = memberships.find(member.char_id);
      if (previous == memberships.end() || previous->second != member.guild_id) {
        advance_quest_membership_generation(generations, member.char_id);
      }
      memberships[member.char_id] = member.guild_id;
    }
  }
  for (const auto &member : members) {
    staged_members[member.char_id] = member;
  }
  if (before_membership_publish_for_test_) {
    before_membership_publish_for_test_();
  }
  members_.swap(staged_members);
  if (quest_services_) {
    quest_services_->guild_memberships.swap(memberships);
    quest_services_->guild_generations.swap(generations);
  }
}

void MemoryGuildStore::remove_member(std::int64_t guild_id, std::int64_t char_id,
                                     const GuildHistory &history) {
  std::unique_lock<std::mutex> coordinator_lock;
  if (quest_services_) {
    coordinator_lock = std::unique_lock(quest_services_->mutex);
    require_memory_owner_epoch(*quest_services_, owner_epoch_.load());
    if (quest_services_->guild_effect_pending(char_id) ||
        quest_services_->guild_effect_pending_for_guild(guild_id)) {
      throw std::runtime_error("Guild departure waits for a pending quest reward");
    }
  }
  std::lock_guard lock(mutex_);
  const auto found = members_.find(char_id);
  if (found == members_.end() || found->second.guild_id != guild_id) {
    throw std::runtime_error("Guild membership to remove does not exist");
  }

  auto memberships = quest_services_ ? quest_services_->guild_memberships
                                     : std::map<std::int64_t, std::int64_t>{};
  auto generations = quest_services_ ? quest_services_->guild_generations
                                     : std::map<std::int64_t, std::int64_t>{};
  auto staged_members = members_;
  auto staged_histories = histories_;
  if (quest_services_) {
    advance_quest_membership_generation(generations, char_id);
  }
  memberships.erase(char_id);
  staged_members.erase(char_id);
  staged_histories[history.char_id] = history;
  if (before_membership_publish_for_test_) {
    before_membership_publish_for_test_();
  }
  members_.swap(staged_members);
  histories_.swap(staged_histories);
  if (quest_services_) {
    quest_services_->guild_memberships.swap(memberships);
    quest_services_->guild_generations.swap(generations);
  }
}

void MemoryGuildStore::save_history(const GuildHistory &history) {
  std::unique_lock<std::mutex> coordinator_lock;
  if (quest_services_) {
    coordinator_lock = std::unique_lock(quest_services_->mutex);
    require_memory_owner_epoch(*quest_services_, owner_epoch_.load());
  }
  std::lock_guard lock(mutex_);
  histories_[history.char_id] = history;
}

void MemoryGuildStore::delete_guild(std::int64_t guild_id,
                                    const std::vector<GuildHistory> &histories) {
  std::unique_lock<std::mutex> coordinator_lock;
  if (quest_services_) {
    coordinator_lock = std::unique_lock(quest_services_->mutex);
    require_memory_owner_epoch(*quest_services_, owner_epoch_.load());
    if (quest_services_->guild_effect_pending_for_guild(guild_id)) {
      throw std::runtime_error("Guild disband waits for a pending quest reward");
    }
  }
  std::lock_guard lock(mutex_);
  auto memberships = quest_services_ ? quest_services_->guild_memberships
                                     : std::map<std::int64_t, std::int64_t>{};
  auto generations = quest_services_ ? quest_services_->guild_generations
                                     : std::map<std::int64_t, std::int64_t>{};
  auto staged_guilds = guilds_;
  auto staged_members = members_;
  auto staged_histories = histories_;
  if (quest_services_) {
    for (const auto &[character_id, current_guild] : memberships) {
      if (current_guild == guild_id) {
        advance_quest_membership_generation(generations, character_id);
      }
    }
  }
  std::erase_if(memberships, [guild_id](const auto &entry) {
    return entry.second == guild_id;
  });
  if (staged_guilds.erase(guild_id) == 0) {
    throw std::runtime_error("Guild to delete does not exist");
  }

  std::erase_if(staged_members, [guild_id](const auto &entry) {
    return entry.second.guild_id == guild_id;
  });
  for (const auto &history : histories) {
    staged_histories[history.char_id] = history;
  }
  if (before_membership_publish_for_test_) {
    before_membership_publish_for_test_();
  }
  guilds_.swap(staged_guilds);
  members_.swap(staged_members);
  histories_.swap(staged_histories);
  if (quest_services_) {
    quest_services_->guild_memberships.swap(memberships);
    quest_services_->guild_generations.swap(generations);
  }
}

PostgresGuildStore::PostgresGuildStore(std::string connection)
    : connection_(std::move(connection)) {}

void PostgresGuildStore::adopt_quest_service_owner_epoch(std::int64_t epoch) {
  if (epoch <= 0) {
    throw std::invalid_argument("Invalid guild store owner epoch");
  }
  owner_epoch_.store(epoch);
}

void PostgresGuildStore::commit_progression(const GuildRecord &before_guild,
                                            const GuildMemberRecord &before_member,
                                            const GuildRecord &after_guild,
                                            const GuildMemberRecord &after_member) {
  validate_progression_update(before_guild, before_member, after_guild, after_member);
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  detail::require_reward_owner_epoch(transaction, owner_epoch_.load());
  detail::lock_quest_service_guild(transaction, before_guild.id);
  detail::lock_quest_service_character(transaction, before_member.char_id);
  detail::require_quest_guild_write_allowed(transaction, before_guild.id);
  require_character_guild_write_allowed(transaction, before_member.char_id);
  const auto rows = transaction.exec(
      "SELECT g.experience,g.guild_level,m.contribution FROM emulator_guilds g "
      "JOIN emulator_guild_members m ON m.guild_id=g.id "
      "WHERE g.id=$1 AND m.character_id=$2 FOR UPDATE OF g,m",
      pqxx::params{before_guild.id, before_member.char_id});
  if (rows.size() != 1 || rows.front()[0].as<int>() != before_guild.experience ||
      rows.front()[1].as<int>() != before_guild.level ||
      rows.front()[2].as<int>() != before_member.contribution) {
    throw std::runtime_error("Guild progression changed before commit");
  }
  transaction.exec("UPDATE emulator_guilds SET experience=$2,guild_level=$3 WHERE id=$1",
                   pqxx::params{before_guild.id, after_guild.experience, after_guild.level});
  transaction.exec("UPDATE emulator_guild_members SET contribution=$2 WHERE character_id=$1",
                   pqxx::params{before_member.char_id, after_member.contribution});
  transaction.commit();
}

GuildProgressionSnapshot PostgresGuildStore::read_progression(
    int world_id, std::int64_t guild_id, const std::optional<std::int64_t> &character_id) {
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  detail::require_reward_owner_epoch(transaction, owner_epoch_.load());
  detail::lock_quest_service_guild(transaction, guild_id);
  if (character_id) {
    detail::lock_quest_service_character(transaction, *character_id);
  }
  const auto guild = transaction.exec(
      "SELECT name,description,notice,notice_character_id,notice_epoch_ticks,join_type,"
      "join_level,join_company_position,experience,guild_level,created_epoch_ticks,"
      "join_company_type FROM emulator_guilds WHERE id=$1 AND world_id=$2 FOR UPDATE",
      pqxx::params{guild_id, world_id});
  if (guild.size() != 1) {
    throw std::runtime_error("Guild progression readback lost its guild");
  }
  GuildProgressionSnapshot result;
  result.guild.id = guild_id;
  result.guild.world_id = world_id;
  const auto &row = guild.front();
  result.guild.name = row[0].as<std::string>();
  result.guild.description = row[1].as<std::string>();
  result.guild.notice = row[2].as<std::string>();
  result.guild.notice_char_id = row[3].as<std::int64_t>();
  result.guild.notice_epoch_ticks = row[4].as<std::int64_t>();
  result.guild.join_type = stored_join_type(row[5].as<int>());
  result.guild.join_level = row[6].as<int>();
  result.guild.join_company_position = row[7].as<int>();
  result.guild.experience = row[8].as<int>();
  result.guild.level = row[9].as<int>();
  result.guild.created_epoch_ticks = row[10].as<std::int64_t>();
  result.guild.join_company_type = row[11].as<int>();
  if (character_id) {
    const auto member = transaction.exec(
        "SELECT contribution FROM emulator_guild_members "
        "WHERE guild_id=$1 AND character_id=$2 FOR UPDATE",
        pqxx::params{guild_id, *character_id});
    if (member.size() != 1) {
      throw std::runtime_error("Guild progression readback lost its member");
    }
    result.member.emplace();
    result.member->guild_id = guild_id;
    result.member->char_id = *character_id;
    result.member->contribution = member.front()[0].as<int>();
  }
  transaction.commit();
  return result;
}

GuildQuestEffectResult PostgresGuildStore::read_quest_effect(
    const QuestServiceEffectEvent &event) {
  return quest_effect(event, nullptr, nullptr, nullptr, nullptr, 0, 0);
}

GuildQuestEffectResult PostgresGuildStore::settle_quest_effect(
    const QuestServiceEffectEvent &event, const GuildRecord &before_guild,
    const GuildMemberRecord &before_member, const GuildRecord &after_guild,
    const GuildMemberRecord &after_member, std::int64_t before_remainder,
    std::int64_t after_remainder) {
  return quest_effect(event, &before_guild, &before_member, &after_guild, &after_member,
                      before_remainder, after_remainder);
}

GuildQuestEffectResult PostgresGuildStore::quest_effect(
    const QuestServiceEffectEvent &event, const GuildRecord *before_guild,
    const GuildMemberRecord *before_member, const GuildRecord *after_guild,
    const GuildMemberRecord *after_member, std::int64_t before_remainder,
    std::int64_t after_remainder) {
  if (event.effects.expected_guild_id <= 0 || event.character_id <= 0 ||
      (event.effects.guild_experience <= 0 && event.effects.activity_experience <= 0) ||
      (event.effects.activity_experience > 0 && event.effects.activity_guild_rate <= 0)) {
    return {QuestEffectSettlementStatus::identity_mismatch};
  }
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  detail::require_reward_owner_epoch(transaction, owner_epoch_.load());
  detail::lock_quest_service_guild(transaction, event.effects.expected_guild_id);
  detail::lock_quest_service_character(transaction, event.character_id);
  const auto rows = transaction.exec(
      "SELECT enqueue_sequence,account_id,world_id,mutation_fingerprint,monster_receipt,"
      "effect_payload,granted_items,guild_applied,expected_company_type,expected_guild_id,"
      "expected_company_generation,expected_guild_generation "
      ",company_applied "
      "FROM emulator_quest_service_effects "
      "WHERE character_id=$1 AND effect_id=$2 FOR UPDATE",
      pqxx::params{event.character_id, event.effect_id});
  if (rows.size() != 1) {
    return {QuestEffectSettlementStatus::identity_mismatch};
  }
  const auto &row = rows.front();
  if (row[0].as<std::int64_t>() <= 0 ||
      static_cast<std::uint64_t>(row[0].as<std::int64_t>()) != event.enqueue_sequence ||
      row[1].as<std::int64_t>() != event.account_id || row[2].as<int>() != event.world_id ||
      row[3].as<std::string>() != event.mutation_fingerprint ||
      row[4].as<bool>() != event.monster_receipt ||
      row[8].as<int>() != event.effects.expected_company_type ||
      row[9].as<std::int64_t>() != event.effects.expected_guild_id ||
      row[10].as<std::int64_t>() != event.effects.expected_company_generation ||
      row[11].as<std::int64_t>() != event.effects.expected_guild_generation ||
      decode_quest_service_effects(nlohmann::json::parse(row[5].as<std::string>())) !=
          event.effects ||
      decode_quest_granted_items(nlohmann::json::parse(row[6].as<std::string>())) !=
          event.granted_items) {
    return {QuestEffectSettlementStatus::identity_mismatch};
  }
  const auto values = transaction.exec(
      "SELECT g.experience,g.guild_level,m.contribution,c.account_id,guard.guild_generation "
      "FROM emulator_guilds g JOIN emulator_guild_members m ON m.guild_id=g.id "
      "JOIN emulator_characters c ON c.id=m.character_id "
      "JOIN emulator_quest_service_character_guards guard ON guard.character_id=c.id "
      "WHERE g.id=$1 AND g.world_id=$2 AND m.character_id=$3 AND c.world_id=$2 "
      "FOR UPDATE OF g,m",
      pqxx::params{event.effects.expected_guild_id, event.world_id, event.character_id});
  if (values.size() != 1 || values.front()[3].as<std::int64_t>() != event.account_id ||
      values.front()[4].as<std::int64_t>() != event.effects.expected_guild_generation) {
    return {QuestEffectSettlementStatus::identity_mismatch};
  }
  GuildQuestEffectResult result;
  result.guild.emplace();
  result.guild->id = event.effects.expected_guild_id;
  result.guild->world_id = event.world_id;
  result.guild->experience = values.front()[0].as<int>();
  result.guild->level = values.front()[1].as<int>();
  result.member.emplace();
  result.member->guild_id = result.guild->id;
  result.member->char_id = event.character_id;
  result.member->account_id = event.account_id;
  result.member->contribution = values.front()[2].as<int>();
  result.event_completed = row[7].as<bool>() && row[12].as<bool>();
  if (event.effects.activity_experience > 0) {
    const auto remainder = transaction.exec(
        "SELECT remainder FROM emulator_quest_activity_remainders "
        "WHERE world_id=$1 AND character_id=$2 AND component=1 "
        "AND membership_generation=$3 AND conversion_rate=$4",
        pqxx::params{event.world_id, event.character_id,
                     event.effects.expected_guild_generation, event.effects.activity_guild_rate});
    if (!remainder.empty()) {
      result.activity_remainder = remainder.front()[0].as<std::int64_t>();
    }
  }
  if (row[7].as<bool>()) {
    result.status = QuestEffectSettlementStatus::already_applied;
    transaction.commit();
    return result;
  }
  detail::require_quest_effect_is_first(transaction, event.character_id, event.effect_id);
  detail::require_quest_guild_write_allowed(
      transaction, result.guild->id, std::make_pair(event.character_id, event.effect_id));
  result.status = QuestEffectSettlementStatus::pending;
  if (before_guild == nullptr) {
    transaction.commit();
    return result;
  }
  if (before_guild->id != result.guild->id || after_guild->id != before_guild->id ||
      before_member->char_id != event.character_id ||
      after_member->char_id != before_member->char_id ||
      before_guild->experience != result.guild->experience ||
      before_guild->level != result.guild->level ||
      before_member->contribution != result.member->contribution ||
      before_remainder != result.activity_remainder ||
      !valid_activity_remainders(event, before_remainder, after_remainder) ||
      after_guild->experience < 0 || after_guild->level < 1 ||
      after_member->contribution < before_member->contribution) {
    result.status = QuestEffectSettlementStatus::retry;
    return result;
  }
  transaction.exec("UPDATE emulator_guilds SET experience=$2,guild_level=$3 WHERE id=$1",
                   pqxx::params{result.guild->id, after_guild->experience, after_guild->level});
  transaction.exec("UPDATE emulator_guild_members SET contribution=$2 WHERE character_id=$1",
                   pqxx::params{event.character_id, after_member->contribution});
  if (event.effects.activity_experience > 0) {
    transaction.exec(
        "INSERT INTO emulator_quest_activity_remainders "
        "(world_id,character_id,component,membership_generation,conversion_rate,remainder) "
        "VALUES ($1,$2,1,$3,$4,$5) "
        "ON CONFLICT (world_id,character_id,component,membership_generation,conversion_rate) "
        "DO UPDATE SET remainder=EXCLUDED.remainder",
        pqxx::params{event.world_id, event.character_id,
                     event.effects.expected_guild_generation, event.effects.activity_guild_rate,
                     after_remainder});
    result.activity_remainder = after_remainder;
  }
  transaction.exec("UPDATE emulator_quest_service_effects SET guild_applied=true "
                   "WHERE character_id=$1 AND effect_id=$2",
                   pqxx::params{event.character_id, event.effect_id});
  result.guild->experience = after_guild->experience;
  result.guild->level = after_guild->level;
  result.member->contribution = after_member->contribution;
  result.status = QuestEffectSettlementStatus::applied;
  result.event_completed = row[12].as<bool>();
  transaction.commit();
  return result;
}

GuildSnapshot PostgresGuildStore::load(int world_id) {
  pqxx::connection connection(connection_);
  pqxx::read_transaction transaction(connection);
  GuildSnapshot snapshot;

  const auto guild_rows = transaction.exec(
      "SELECT id,world_id,name,description,notice,notice_character_id,notice_epoch_ticks,"
      "join_type,join_level,join_company_position,experience,guild_level,created_epoch_ticks,"
      "join_company_type FROM emulator_guilds WHERE world_id=$1 ORDER BY id",
      pqxx::params{world_id});
  for (const auto &row : guild_rows) {
    GuildRecord guild;
    guild.id = row[0].as<std::int64_t>();
    guild.world_id = row[1].as<int>();
    guild.name = row[2].as<std::string>();
    guild.description = row[3].as<std::string>();
    guild.notice = row[4].as<std::string>();
    guild.notice_char_id = row[5].as<std::int64_t>();
    guild.notice_epoch_ticks = row[6].as<std::int64_t>();
    guild.join_type = stored_join_type(row[7].as<int>());
    guild.join_level = row[8].as<int>();
    guild.join_company_position = row[9].as<int>();
    guild.experience = row[10].as<int>();
    guild.level = row[11].as<int>();
    guild.created_epoch_ticks = row[12].as<std::int64_t>();
    guild.join_company_type = row[13].as<int>();
    snapshot.guilds.push_back(std::move(guild));
  }

  // Display fields come from the character row, the authority for name, level and class.
  const auto member_rows = transaction.exec(
      "SELECT m.guild_id,m.character_id,m.member_grade,c.name,c.level,c.class_id,"
      "m.contribution,m.joined_epoch_ticks,m.last_connect_epoch_ticks,m.company_position,"
      "c.account_id FROM emulator_guild_members m "
      "JOIN emulator_guilds g ON g.id=m.guild_id "
      "JOIN emulator_characters c ON c.id=m.character_id "
      "WHERE g.world_id=$1 ORDER BY m.guild_id,m.character_id",
      pqxx::params{world_id});
  for (const auto &row : member_rows) {
    GuildMemberRecord member;
    member.guild_id = row[0].as<std::int64_t>();
    member.char_id = row[1].as<std::int64_t>();
    member.grade = stored_grade(row[2].as<int>());
    member.name = row[3].as<std::string>();
    member.level = row[4].as<int>();
    member.char_class = row[5].as<int>();
    member.contribution = row[6].as<int>();
    member.joined_epoch_ticks = row[7].as<std::int64_t>();
    member.last_connect_epoch_ticks = row[8].as<std::int64_t>();
    member.company_position = row[9].as<int>();
    member.account_id = row[10].as<std::int64_t>();
    snapshot.members.push_back(std::move(member));
  }

  const auto history_rows = transaction.exec(
      "SELECT h.character_id,h.last_leave_epoch_ticks,h.last_disband_epoch_ticks "
      "FROM emulator_character_guild_history h "
      "JOIN emulator_characters c ON c.id=h.character_id WHERE c.world_id=$1",
      pqxx::params{world_id});
  for (const auto &row : history_rows) {
    GuildHistory history;
    history.char_id = row[0].as<std::int64_t>();
    history.last_leave_epoch_ticks = row[1].as<std::int64_t>();
    history.last_disband_epoch_ticks = row[2].as<std::int64_t>();
    snapshot.histories.push_back(history);
  }

  return snapshot;
}

std::optional<GuildRecord> PostgresGuildStore::create_guild(GuildRecord guild,
                                                            const GuildMemberRecord &master) {
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  detail::require_reward_owner_epoch(transaction, owner_epoch_.load());
  detail::lock_quest_service_character(transaction, master.char_id);
  require_character_guild_write_allowed(transaction, master.char_id);

  // The only unique constraint besides the identity key is the realm/lower(name) index.
  const auto inserted = transaction.exec(
      "INSERT INTO emulator_guilds(world_id,name,description,notice,notice_character_id,"
      "notice_epoch_ticks,join_type,join_level,join_company_position,experience,guild_level,"
      "created_epoch_ticks,join_company_type) "
      "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13) "
      "ON CONFLICT DO NOTHING RETURNING id",
      pqxx::params{guild.world_id, guild.name, guild.description, guild.notice,
                   guild.notice_char_id, guild.notice_epoch_ticks,
                   static_cast<int>(guild.join_type), guild.join_level,
                   guild.join_company_position, guild.experience, guild.level,
                   guild.created_epoch_ticks, guild.join_company_type});
  if (inserted.empty()) {
    return std::nullopt;
  }
  guild.id = inserted.front()[0].as<std::int64_t>();

  transaction.exec(
      "INSERT INTO emulator_guild_members(character_id,guild_id,member_grade,contribution,"
      "joined_epoch_ticks,last_connect_epoch_ticks,company_position) "
      "VALUES ($1,$2,$3,$4,$5,$6,$7)",
      pqxx::params{master.char_id, guild.id, static_cast<int>(GuildGrade::master),
                   master.contribution, master.joined_epoch_ticks,
                   master.last_connect_epoch_ticks, master.company_position});
  detail::advance_quest_guild_generation(transaction, master.char_id);
  transaction.commit();
  return guild;
}

void PostgresGuildStore::update_guild(const GuildRecord &guild) {
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  detail::require_reward_owner_epoch(transaction, owner_epoch_.load());
  detail::lock_quest_service_guild(transaction, guild.id);
  detail::require_quest_guild_write_allowed(transaction, guild.id);
  const auto updated = transaction.exec(
      "UPDATE emulator_guilds SET description=$2,notice=$3,notice_character_id=$4,"
      "notice_epoch_ticks=$5,join_type=$6,join_level=$7,join_company_position=$8,"
      "experience=$9,guild_level=$10 WHERE id=$1 RETURNING id",
      pqxx::params{guild.id, guild.description, guild.notice, guild.notice_char_id,
                   guild.notice_epoch_ticks, static_cast<int>(guild.join_type), guild.join_level,
                   guild.join_company_position, guild.experience, guild.level});
  if (updated.empty()) {
    throw std::runtime_error("Guild to update does not exist");
  }
  transaction.commit();
}

void PostgresGuildStore::save_members(const std::vector<GuildMemberRecord> &members) {
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  detail::require_reward_owner_epoch(transaction, owner_epoch_.load());
  std::set<std::int64_t> guild_ids;
  std::set<std::int64_t> character_ids;
  for (const auto &member : members) {
    validate_stored_member(member);
    guild_ids.insert(member.guild_id);
    character_ids.insert(member.char_id);
  }
  for (const auto guild_id : guild_ids) {
    detail::lock_quest_service_guild(transaction, guild_id);
  }
  for (const auto character_id : character_ids) {
    detail::lock_quest_service_character(transaction, character_id);
  }
  for (const auto guild_id : guild_ids) {
    detail::require_quest_guild_write_allowed(transaction, guild_id);
  }
  for (const auto character_id : character_ids) {
    require_character_guild_write_allowed(transaction, character_id);
  }
  for (const auto &member : members) {
    const auto existing = transaction.exec(
        "SELECT guild_id FROM emulator_guild_members WHERE character_id=$1",
        pqxx::params{member.char_id});
    // The WHERE clause refuses to move a character between guilds through an upsert.
    const auto saved = transaction.exec(
        "INSERT INTO emulator_guild_members(character_id,guild_id,member_grade,contribution,"
        "joined_epoch_ticks,last_connect_epoch_ticks,company_position) "
        "VALUES ($1,$2,$3,$4,$5,$6,$7) "
        "ON CONFLICT (character_id) DO UPDATE SET member_grade=EXCLUDED.member_grade,"
        "contribution=EXCLUDED.contribution,"
        "last_connect_epoch_ticks=EXCLUDED.last_connect_epoch_ticks,"
        "company_position=EXCLUDED.company_position "
        "WHERE emulator_guild_members.guild_id=EXCLUDED.guild_id RETURNING character_id",
        pqxx::params{member.char_id, member.guild_id, static_cast<int>(member.grade),
                     member.contribution, member.joined_epoch_ticks,
                     member.last_connect_epoch_ticks, member.company_position});
    if (saved.empty()) {
      throw std::runtime_error("Character already belongs to another guild");
    }
    if (existing.empty()) {
      detail::advance_quest_guild_generation(transaction, member.char_id);
    }
  }
  transaction.commit();
}

void PostgresGuildStore::remove_member(std::int64_t guild_id, std::int64_t char_id,
                                       const GuildHistory &history) {
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  detail::require_reward_owner_epoch(transaction, owner_epoch_.load());
  detail::lock_quest_service_guild(transaction, guild_id);
  detail::lock_quest_service_character(transaction, char_id);
  detail::require_quest_guild_write_allowed(transaction, guild_id);
  require_character_guild_write_allowed(transaction, char_id);
  const auto removed = transaction.exec(
      "DELETE FROM emulator_guild_members WHERE guild_id=$1 AND character_id=$2 "
      "RETURNING character_id",
      pqxx::params{guild_id, char_id});
  if (removed.empty()) {
    throw std::runtime_error("Guild membership to remove does not exist");
  }

  detail::advance_quest_guild_generation(transaction, char_id);
  upsert_history(transaction, history);
  transaction.commit();
}

void PostgresGuildStore::delete_guild(std::int64_t guild_id,
                                      const std::vector<GuildHistory> &histories) {
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  detail::require_reward_owner_epoch(transaction, owner_epoch_.load());
  detail::lock_quest_service_guild(transaction, guild_id);
  detail::require_quest_guild_write_allowed(transaction, guild_id);
  const auto memberships = transaction.exec(
      "SELECT character_id FROM emulator_guild_members WHERE guild_id=$1 "
      "ORDER BY character_id",
      pqxx::params{guild_id});
  for (const auto &membership : memberships) {
    const auto character_id = membership[0].as<std::int64_t>();
    detail::lock_quest_service_character(transaction, character_id);
    require_character_guild_write_allowed(transaction, character_id);
  }
  // Memberships cascade with the guild row.
  const auto deleted = transaction.exec(
      "DELETE FROM emulator_guilds WHERE id=$1 RETURNING id", pqxx::params{guild_id});
  if (deleted.empty()) {
    throw std::runtime_error("Guild to delete does not exist");
  }

  for (const auto &membership : memberships) {
    detail::advance_quest_guild_generation(transaction, membership[0].as<std::int64_t>());
  }
  for (const auto &history : histories) {
    upsert_history(transaction, history);
  }
  transaction.commit();
}

void PostgresGuildStore::save_history(const GuildHistory &history) {
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  detail::require_reward_owner_epoch(transaction, owner_epoch_.load());
  upsert_history(transaction, history);
  transaction.commit();
}

} // namespace chronicles
