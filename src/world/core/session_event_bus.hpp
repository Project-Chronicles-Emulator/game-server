#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <functional>
#include <vector>

namespace chronicles {

struct Character;
struct CharacterBuild;
struct WorldProgress;

struct PlayerProtectionChanged {
  bool invincible{};
  bool hidden{};
  bool infinite_stamina{};
};

struct PlayerVisibilityChanged {
  std::uint64_t connection_id{};
  bool hidden{};
};

// A committed/live stat change requires the current player cache to be refreshed.
// Callers retain their original codec-lock scope; subscribers must not acquire it implicitly.
struct PlayerCombatSnapshotChanged {};

struct CompanyMembershipChanged {};

struct CharacterActivityExperienceGained {
  std::int64_t character_id{};
  std::int64_t experience{};
};

// Borrowed payloads are valid only for this synchronous publication. Subscribers may not
// retain references or pointers beyond publish(), including a prepared combat-stat pointer.
struct PlayerProgressCommitted {
  const WorldProgress &progress;
};

struct PlayerInventoryStatsChanged {
  const Character &character;
};

struct PlayerInventorySnapshotChanged {
  const Character &character;
  const nlohmann::json *prepared_stats{};
};

// Inventory publishes after the successful authoritative item revision commit. The tutorial
// subscriber advances only the existing cached revision at the original call position.
struct PlayerEquipmentRevisionCommitted {
  std::int64_t revision{};
};

// Utility-slot saves update this separate loot cache only under their original world-mobs
// guard. The payload is borrowed for synchronous delivery, with no extra progress copy.
struct PlayerWorldLootProgressCommitted {
  const WorldProgress &progress;
};

// Character progression updates refresh the handshake cache at its original codec-locked
// call sites. The bus borrows the build only during delivery. A subscriber may copy it
// into its owned cache, but must not retain the borrowed reference.
struct PlayerProgressionSnapshotChanged {
  int level{};
  std::int64_t experience{};
  const CharacterBuild &build;
};

// A committed level change updates the existing Party identity before the progression cache
// lock and level-up packet effects. The subscriber does not acquire a codec lock.
struct PlayerLevelChanged {
  int level{};
};

// Publish only after a durable death/revival succeeds. Ordinary combat snapshots must not
// update observer-visible life. Stats are borrowed synchronously at the caller's lock scope.
struct PlayerLifeCommitted {
  bool dead{};
  const nlohmann::json &stats;
};

// One bus per player session, used only on its existing worker strand. Composition builds
// every subscription before processing begins. Lists are immutable: no off-strand removal,
// lock, scheduling or feature self-registration can change delivery order during a publish.
class SessionEventBus final {
public:
  template <typename Event> using Subscriber = std::function<void(const Event &)>;

  struct Subscriptions {
    std::vector<Subscriber<PlayerProtectionChanged>> protection;
    std::vector<Subscriber<PlayerVisibilityChanged>> visibility;
    std::vector<Subscriber<PlayerCombatSnapshotChanged>> player_state;
    std::vector<Subscriber<CompanyMembershipChanged>> company;
    std::vector<Subscriber<CharacterActivityExperienceGained>> activity_experience;
    std::vector<Subscriber<PlayerProgressCommitted>> progress;
    std::vector<Subscriber<PlayerInventoryStatsChanged>> inventory_stats;
    std::vector<Subscriber<PlayerInventorySnapshotChanged>> inventory_snapshot;
    std::vector<Subscriber<PlayerEquipmentRevisionCommitted>> equipment_revision;
    std::vector<Subscriber<PlayerWorldLootProgressCommitted>> world_loot_progress;
    std::vector<Subscriber<PlayerProgressionSnapshotChanged>> progression_snapshot;
    std::vector<Subscriber<PlayerLevelChanged>> player_level;
    std::vector<Subscriber<PlayerLifeCommitted>> player_life;
  };

  explicit SessionEventBus(Subscriptions subscriptions);

  SessionEventBus(const SessionEventBus &) = delete;
  SessionEventBus &operator=(const SessionEventBus &) = delete;
  SessionEventBus(SessionEventBus &&) = delete;
  SessionEventBus &operator=(SessionEventBus &&) = delete;

  void publish(const PlayerProtectionChanged &event) const;

  void publish(const PlayerVisibilityChanged &event) const;

  void publish(const PlayerCombatSnapshotChanged &event) const;

  void publish(const CompanyMembershipChanged &event) const;

  void publish(const CharacterActivityExperienceGained &event) const;

  void publish(const PlayerProgressCommitted &event) const;

  void publish(const PlayerInventoryStatsChanged &event) const;

  void publish(const PlayerInventorySnapshotChanged &event) const;

  void publish(const PlayerEquipmentRevisionCommitted &event) const;

  void publish(const PlayerWorldLootProgressCommitted &event) const;

  void publish(const PlayerProgressionSnapshotChanged &event) const;

  void publish(const PlayerLevelChanged &event) const;

  void publish(const PlayerLifeCommitted &event) const;

private:
  // Subscriber closures borrow the containing session and resolve its current targets.
  // The session declares the bus before consumers, so their bus references remain valid
  // through destruction. Publication runs only while its target state is live; pending
  // work retains the containing owner. Bus construction/destruction never invokes a sink.
  const Subscriptions subscriptions_;
};

} // namespace chronicles
