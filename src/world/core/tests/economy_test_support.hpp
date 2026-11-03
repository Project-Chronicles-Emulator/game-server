#pragma once

#include "data/shop_catalog.hpp"
#include "persistence/tests/memory_character_repositories.hpp"
#include "protocol/protocol.hpp"
#include "world/core/progress_access.hpp"
#include "world/core/session_event_bus.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace chronicles::economy_testing {

using Json = nlohmann::json;

inline constexpr int kMaterial = 701;
inline constexpr int kEquipment = 702;
inline constexpr int kHousing = 703;
inline constexpr int kShop = 1001;
inline constexpr int kMerchantNpc = 9001;
inline constexpr int kInteraction = 212;
inline constexpr std::uint64_t kMerchantUnit = 0x80000102;
inline constexpr std::int64_t kNow = 18'000'000'000'000'000;

inline ShopCatalogTables item_tables() {
  ShopCatalogTables tables;
  tables.items = {{{"id", "701"},
                   {"category", "CraftMaterial"},
                   {"grade", "Normal"},
                   {"priceType", "Gold"},
                   {"priceValue", "5"},
                   {"refundType", "Gold"},
                   {"refundValue", "2"},
                   {"weight", "10"}},
                  {{"id", "702"},
                   {"category", "Equipment"},
                   {"grade", "Rare"},
                   {"maxDurability", "10000"},
                   {"priceType", "Gold"},
                   {"priceValue", "100"},
                   {"refundType", "Gold"},
                   {"refundValue", "20"},
                   {"weight", "50"}},
                  {{"id", "703"},
                   {"category", "HousingObject"},
                   {"grade", "Normal"},
                   {"priceType", "Gold"},
                   {"priceValue", "10"},
                   {"refundType", "Gold"},
                   {"refundValue", "2"}}};
  tables.shops = {{{"id", "1001"}, {"FK_itemId", "701"}}, {{"id", "1001"}, {"FK_itemId", "702"}}};
  tables.interactions = {{{"id", "212"},
                          {"groupId", "212"},
                          {"playerTaskCase", "Shop"},
                          {"playerTask", "\"shopId\":1001"}}};
  tables.npcs = {{{"id", "9001"}, {"interactionGroupId", "212"}}};
  tables.game_variables = {{{"name", "ItemMaxStack"}, {"value", "999"}},
                           {{"name", "ResetBaseDateTime"}, {"value", "2025-01-01 00:00:00"}}};
  tables.money = {{{"type", "Gold"}, {"limitUserMax", "9000000000000000"}}};
  return tables;
}

inline Character create_owner(MemoryRepositories &repositories, const std::string &suffix,
                              std::optional<std::int64_t> account_id = std::nullopt, int slot = 0) {
  if (!account_id) {
    Account account;
    account.name = "economy" + suffix;
    account.salt = "test-salt";
    account.hash = "test-hash";
    if (!repositories.accounts->add_account(account)) {
      throw std::runtime_error("Cannot create economy probe account");
    }
    account_id = repositories.accounts->account(account.name)->id;
  }
  Character character;
  character.account_id = *account_id;
  character.name = "Econ" + suffix;
  character.world_id = 1;
  character.class_id = 2;
  character.level = 1;
  character.slot = slot;
  character.world_position = WorldPosition{8000, 0, 0, 0, 0.0F};
  return repositories.characters->create(character);
}

class RecordingProgress final : public ProgressAccess {
public:
  RecordingProgress(MemoryRepositories &repositories, const Character &owner,
                    std::vector<std::string> &trace)
      : backend_(*repositories.characters, *repositories.character_state, owner.account_id,
                 owner.id, owner.world_id),
        trace_(trace) {
    cached = backend_.load();
  }

  WorldProgress load() override {
    trace_.push_back("load");
    return use_cache ? cached : backend_.load();
  }

  std::optional<Character> character() override {
    trace_.push_back("character");
    if (throw_character) {
      throw std::runtime_error("Synthetic actor read failure");
    }
    if (missing_character) {
      return std::nullopt;
    }
    return backend_.character();
  }

  std::optional<std::vector<CharacterItem>>
  commit(const WorldProgress &before, const WorldProgress &after, const ItemTrade &trade) override {
    trace_.push_back("commit");
    if (throw_commit) {
      throw std::runtime_error("Synthetic item commit failure");
    }
    if (reject_commit) {
      return std::nullopt;
    }
    return backend_.commit(before, after, trade);
  }

  bool use_cache{true};
  bool missing_character{};
  bool throw_character{};
  bool reject_commit{};
  bool throw_commit{};
  WorldProgress cached;

private:
  RepositoryProgressAccess backend_;
  std::vector<std::string> &trace_;
};

class EconomyFixture {
public:
  EconomyFixture() {
    actor = create_owner(repositories, "owner");
    const auto before = repositories.character_state->load_world_progress(actor.account_id,
                                                                          actor.id, actor.world_id);
    auto after = before;
    ++after.revision;
    after.gold = 5000;
    after.stackables[kMaterial] = 20;
    if (!repositories.character_state->save_world_progress(actor.account_id, actor.id,
                                                           actor.world_id, before, after)) {
      throw std::runtime_error("Cannot fund economy probe actor");
    }
    progress = std::make_unique<RecordingProgress>(repositories, actor, trace);
    items = std::make_shared<const ShopCatalog>(item_tables());

    SessionEventBus::Subscriptions subscriptions;
    subscriptions.progress.push_back([this](const PlayerProgressCommitted &event) {
      trace.push_back("adopt");
      progress->cached = event.progress;
    });
    subscriptions.inventory_stats.push_back([this](const PlayerInventoryStatsChanged &) {
      trace.push_back("stats");
      prepared_stats["refreshed"] = true;
    });
    subscriptions.inventory_snapshot.push_back([this](const PlayerInventorySnapshotChanged &event) {
      trace.push_back("snapshot");
      last_snapshot = event.character;
      if (on_snapshot) {
        on_snapshot(event);
      }
    });
    events = std::make_unique<SessionEventBus>(std::move(subscriptions));
  }

  WorldProgress stored_progress() const {
    return repositories.character_state->load_world_progress(actor.account_id, actor.id,
                                                             actor.world_id);
  }

  std::size_t trace_index(const std::string &step) const {
    const auto found = std::ranges::find(trace, step);
    if (found == trace.end()) {
      throw std::runtime_error("Missing economy probe step: " + step);
    }
    return static_cast<std::size_t>(std::distance(trace.begin(), found));
  }

  MemoryRepositories repositories;
  Character actor;
  std::vector<std::string> trace;
  std::unique_ptr<RecordingProgress> progress;
  std::unique_ptr<SessionEventBus> events;
  std::shared_ptr<const ShopCatalog> items;
  Json prepared_stats = {{"refreshed", false}};
  std::optional<Character> last_snapshot;
  std::function<void(const PlayerInventorySnapshotChanged &)> on_snapshot;
};

inline Frame encoded_probe_frame(std::uint16_t opcode) {
  Frame frame;
  frame.bytes = {4, 0, static_cast<std::uint8_t>(opcode & 0xff),
                 static_cast<std::uint8_t>(opcode >> 8)};
  return frame;
}

} // namespace chronicles::economy_testing
