#pragma once

#include "world/core/session_event_bus.hpp"
#include "world/features/combat/combat_player_session.hpp"
#include "world/features/combat/tests/combat_feature_test_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <new>

namespace chronicles::combat::testing_support {

class PlayerContext final : public CombatPlayerContext {
public:
  PlayerContext() : events(subscriptions()) {
    actor.id = 19;
    actor.class_id = 2;
    actor.level = 1;
    actor.name = "CombatOwner";
    actor.world_position = kPosition;
  }

  bool combat_is_raid_map(int map_id) const override {
    return admission.combat_is_raid_map(map_id);
  }

  std::optional<std::uint64_t> combat_raid_instance_owner(std::int64_t character_id,
                                                          int map_id) const override {
    return admission.combat_raid_instance_owner(character_id, map_id);
  }

  bool combat_has_handoff() const override {
    return handoff;
  }

  std::int64_t combat_character_id() const override {
    return actor.id;
  }

  std::uint64_t combat_connection_id() const override {
    return 41;
  }

  std::uint64_t combat_wire_player_id(std::int64_t character_id) const override {
    return (static_cast<std::uint64_t>(character_id) << 4) | 1;
  }

  std::int64_t combat_monotonic_ticks() const override {
    return ticks;
  }

  bool combat_closed() const override {
    return closed;
  }

  bool combat_services_entered() const override {
    return entered;
  }

  bool combat_in_housing() const override {
    return housing;
  }

  bool combat_riding() const override {
    return riding;
  }

  bool combat_other_action_busy() const override {
    return other_busy;
  }

  bool combat_has_tutorial() const override {
    return tutorial;
  }

  const WorldProgress &combat_tutorial_progress() const override {
    return progress;
  }

  const std::shared_ptr<const TutorialData> &combat_base_tutorial_data() const override {
    return tutorial_data;
  }

  void combat_accept_tutorial_progress(WorldProgress &&committed) override {
    calls.push_back("tutorial_adopt");
    adopted_progress = std::move(committed);
  }

  std::vector<GameplayPacket> combat_advance_tutorial(CombatSession &, std::int64_t,
                                                      const WorldPosition &) override {
    if (tutorial_packets) {
      return *tutorial_packets;
    }
    throw std::logic_error("Unexpected tutorial advancement in this player-owner fixture");
  }

  std::vector<GameplayPacket> combat_interrupt_gathering(std::int64_t) override {
    ++gathering_interruptions;
    return {{167, Json::object()}};
  }

  ItemInventoryAccess combat_item_inventory() override {
    calls.push_back("item_inventory");
    ItemInventoryAccess access;
    access.quantity = [this](int item_cms_id) {
      return progress.stackables[item_cms_id];
    };
    access.consume_one = [this](int item_cms_id) -> std::vector<GameplayPacket> {
      --progress.stackables[item_cms_id];
      return {};
    };
    return access;
  }

  bool combat_has_character_stats() const override {
    return has_stats;
  }

  Json combat_character_stats(const Character &) const override {
    ++stat_reads;
    return stats;
  }

  std::optional<Character> combat_character() const override {
    ++character_reads;
    return character_exists ? std::optional<Character>(actor) : std::nullopt;
  }

  WorldProgress combat_stored_progress() const override {
    ++progress_reads;
    if (throw_next_progress_read) {
      throw_next_progress_read = false;
      throw std::bad_alloc();
    }
    return progress;
  }

  std::optional<WorldPosition> combat_last_position() const override {
    return last_position;
  }

  bool combat_commit_death(std::int64_t revision,
                           const std::vector<EquippedDurabilityChange> &changes) override {
    calls.push_back("death_commit");
    death_changes = changes;
    if (throw_before_death_commit) {
      throw_before_death_commit = false;
      throw std::runtime_error("Synthetic death write failure before commit");
    }
    if (!commit_succeeds || revision != progress.revision) {
      return false;
    }
    if (apply_death_wear) {
      for (const auto &change : changes) {
        const auto item = std::ranges::find(actor.items, change.item_id, &CharacterItem::id);
        if (item == actor.items.end() || item->durability != change.expected_durability) {
          return false;
        }
      }
      for (const auto &change : changes) {
        const auto item = std::ranges::find(actor.items, change.item_id, &CharacterItem::id);
        item->durability = change.durability;
      }
    }
    ++progress.revision;
    progress.dead = true;
    if (lose_death_ack) {
      lose_death_ack = false;
      throw std::runtime_error("Synthetic death commit acknowledgement lost");
    }
    return true;
  }

  bool combat_register_pending_death() noexcept override {
    if (!death_registration_allowed || death_registered) {
      return false;
    }
    death_registered = true;
    return true;
  }

  void combat_pending_death_updated() noexcept override {
    ++death_recovery_updates;
  }

  bool combat_forget_pending_death() noexcept override {
    if (!death_registered || !death_forget_allowed) {
      return false;
    }
    death_registered = false;
    return true;
  }

  std::optional<DeathReconciliationSnapshot> combat_fence_death_reconciliation() override {
    calls.push_back("death_fence");
    ++progress.revision;
    if (lose_fence_ack) {
      lose_fence_ack = false;
      throw std::runtime_error("Synthetic death fence acknowledgement lost");
    }
    DeathReconciliationSnapshot snapshot;
    snapshot.progress = progress;
    for (const auto &item : actor.items) {
      if (item.equip_slot) {
        snapshot.equipped_items.push_back(item);
      }
    }
    return snapshot;
  }

  bool combat_commit_revival(std::int64_t revision, const WorldPosition &position) override {
    calls.push_back("revival_commit");
    if (!commit_succeeds || revision != progress.revision) {
      return false;
    }
    ++progress.revision;
    progress.dead = false;
    committed_position = position;
    return true;
  }

  bool combat_commit_shrine_revival(const WorldProgress &before, const WorldProgress &after,
                                    const WorldPosition &position) override {
    calls.push_back("shrine_commit");
    shrine_candidate = after;
    if (!commit_succeeds || before != progress) {
      return false;
    }
    progress = after;
    committed_position = position;
    return true;
  }

  bool combat_has_revival_supplies() const override {
    return has_supplies;
  }

  std::vector<GameplayPacket> combat_revival_supplies(int, WorldProgress &candidate,
                                                      const Character &) const override {
    calls.push_back("supplies");
    if (supplies_throw) {
      throw std::runtime_error("Refused fixture supplies");
    }
    candidate.stackables[101] = 3;
    return {{169, Json::object()}};
  }

  std::vector<GameplayPacket> combat_revive_at_shrine(const WorldGameplayRequest &) override {
    calls.push_back("shrine_request");
    return {};
  }

  void combat_attach_world_loot() override {
    calls.push_back("loot_attach");
    ASSERT_TRUE(player->has_world_mobs());
    ++loot_attachments;
  }

  MonsterRewardCapture combat_snapshot_monster_rewards(const MonsterDefeat &) override {
    return {};
  }

  bool combat_enqueue_monster_rewards(const std::vector<MonsterRewardReceipt> &) override {
    return false;
  }

  bool combat_mark_monster_reward_ready(const std::string &) override {
    return false;
  }

  void combat_notify_monster_rewards(const std::vector<MonsterRewardReceipt> &) override {
  }

  void combat_schedule_monster_reward_retry() override {
  }

  void combat_record_monster_reward(const WorldPosition &, const MonsterDefeat &,
                                    std::vector<GameplayPacket> &) override {
    calls.push_back("monster_reward");
  }

  void combat_sync_protection() override {
    calls.push_back("protection");
  }

  std::optional<std::int64_t> combat_next_gameplay_tick() const override {
    return player->next_action_tick();
  }

  void combat_publish_snapshot() override {
    events.publish(PlayerCombatSnapshotChanged{});
  }

  void combat_publish_committed_life(bool dead, const Json &current_stats) override {
    events.publish(PlayerLifeCommitted{dead, current_stats});
  }

  void combat_send_owner(const GameplayPacket &packet) override {
    calls.push_back("owner");
    if (send_owner_throws) {
      throw std::runtime_error("Refused fixture encoding");
    }
    owner_packets.push_back(packet);
  }

  void combat_send_observers(const std::vector<GameplayPacket> &packets) override {
    calls.push_back("observers");
    observer_packets.insert(observer_packets.end(), packets.begin(), packets.end());
  }

  std::vector<GameplayPacket> combat_select_weapon_for_skill(const Json &slot) override {
    calls.push_back("weapon_selection");
    weapon_selections.push_back(slot);
    return on_weapon_selection ? on_weapon_selection(slot) : std::vector<GameplayPacket>{};
  }

  SessionEventBus::Subscriptions subscriptions() {
    SessionEventBus::Subscriptions subscriptions;
    subscriptions.player_state.push_back([this](const PlayerCombatSnapshotChanged &) {
      calls.push_back("snapshot");
      snapshot = player->current_simulation()->player_stats();
    });
    subscriptions.player_life.push_back([this](const PlayerLifeCommitted &event) {
      calls.push_back("presence");
      EXPECT_EQ(event.dead, progress.dead);
      EXPECT_EQ(&event.stats, &player->current_simulation()->player_stats());
      if (on_life) {
        on_life(event);
      }
    });
    return subscriptions;
  }

  Admission admission;
  Character actor;
  WorldProgress progress;
  std::optional<WorldProgress> adopted_progress;
  std::optional<WorldProgress> shrine_candidate;
  bool handoff{true};
  bool closed{};
  bool entered{true};
  bool housing{};
  bool riding{};
  bool other_busy{};
  bool tutorial{};
  std::optional<std::vector<GameplayPacket>> tutorial_packets;
  int gathering_interruptions{};
  bool has_stats{true};
  bool character_exists{true};
  mutable bool throw_next_progress_read{};
  bool commit_succeeds{true};
  bool apply_death_wear{};
  bool death_registration_allowed{true};
  bool death_registered{};
  bool death_forget_allowed{true};
  int death_recovery_updates{};
  bool throw_before_death_commit{};
  bool lose_death_ack{};
  bool lose_fence_ack{};
  bool has_supplies{true};
  bool supplies_throw{};
  bool send_owner_throws{};
  std::int64_t ticks{kStart};
  std::shared_ptr<const TutorialData> tutorial_data;
  Json stats{{"18", 1000}, {"19", 100}, {"69", 1000}, {"72", 100}, {"15", 0}, {"25", 0}};
  Json snapshot;
  std::optional<WorldPosition> last_position;
  std::optional<WorldPosition> committed_position;
  std::vector<EquippedDurabilityChange> death_changes;
  mutable std::vector<std::string> calls;
  mutable int stat_reads{};
  mutable int character_reads{};
  mutable int progress_reads{};
  int loot_attachments{};
  std::vector<GameplayPacket> owner_packets;
  std::vector<GameplayPacket> observer_packets;
  std::function<void(const PlayerLifeCommitted &)> on_life;
  std::vector<Json> weapon_selections;
  std::function<std::vector<GameplayPacket>(const Json &)> on_weapon_selection;
  CombatPlayerSession *player{};
  SessionEventBus events;
};

class PlayerOwner : public ::testing::Test {
protected:
  PlayerOwner() : PlayerOwner(world_profile(Json::array({empty_population(9012)}))) {}

  // Derived fixtures supply their own combat profile; encounters load only once per feature.
  explicit PlayerOwner(Json profile) {
    write_profile(fixture.root(), std::move(profile));
    feature.load_encounters(nullptr, fixture.root() / "catalog");
    feature.load_movement_rules(*catalog);
    player = feature.create_session(context);
    context.player = player.get();
    player->ensure_player_vitals(context.actor, context.progress);
    EXPECT_EQ(context.calls, (std::vector<std::string>{"item_inventory"}));
    context.calls.clear();
  }

  FrameContext move(const char *mode, int height_mm, const char *detail = "MoveDetail_Stop") {
    FrameContext frame(20010, WorldGameplayRequest{20010, 1, Json::object()});
    frame.party_position = kPosition;
    frame.party_position->z_mm = height_mm;
    const std::optional<Json> movement{Json{{"mode", mode}, {"detail", detail}}};
    player->observe_movement_vitals(frame, movement);
    return frame;
  }

  void kill_model() {
    player->current_simulation()->apply_fall_damage(1000, kPosition, context.ticks);
    context.progress.dead = true;
  }

  chronicles::testing_support::TestCatalog fixture{catalog_tables()};
  std::shared_ptr<ClientCatalog> catalog = std::make_shared<ClientCatalog>(fixture.root());
  CombatFeature feature{catalog};
  PlayerContext context;
  std::unique_ptr<CombatPlayerSession> player;
};

} // namespace chronicles::combat::testing_support
