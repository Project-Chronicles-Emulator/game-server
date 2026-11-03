#pragma once

#include "world/core/frame_context.hpp"
#include "world/features/loot/loot.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace chronicles::party {
class PartyWorld;
struct PartyUpdate;
} // namespace chronicles::party

namespace chronicles::loot {

class LootPlayerSession;
class LootSessionContext;

using PartyChannelKey = std::function<std::uint64_t()>;
using PartyLootDelivery = std::function<void(const party::PartyUpdate &)>;

class LootFeature final {
public:
  explicit LootFeature(std::shared_ptr<const LootData> data);
  std::unique_ptr<LootPlayerSession> create_session(LootSessionContext &context);

private:
  friend class LootPlayerSession;

  const std::shared_ptr<const LootData> data_;
  // Shared by the same server's private-instance loot owners, as before extraction.
  const std::shared_ptr<SharedLootBoard> board_ = std::make_shared<SharedLootBoard>();
};

class LootSessionContext {
public:
  virtual ~LootSessionContext() = default;

  // Bootstrap can create private loot before the containing session publishes handoff_.
  // This returns the established adapter explicitly bound to the passed entry identity.
  virtual LootPersistence loot_tutorial_persistence(const Handoff &entry) = 0;
  virtual std::shared_ptr<party::PartyWorld> loot_party_world() = 0;
  // Preserve the existing channel callback and strong-WorldState delivery capture rather
  // than replacing either with a feature/session reference of a different lifetime.
  virtual PartyChannelKey loot_party_channel_key() = 0;
  virtual PartyLootDelivery loot_party_delivery() = 0;
  virtual void loot_install_defeat_observer(MonsterDefeatObserver observer) = 0;

  virtual std::int64_t loot_character_id() const = 0;
  virtual std::optional<Character> loot_stored_character() = 0;
  virtual WorldProgress loot_stored_progress() = 0;
  virtual LootClaimResult loot_lookup_claim(const std::string &receipt_id,
                                             const std::string &corpse_fingerprint) = 0;
  virtual LootClaimResult loot_commit_items(const LootCommit &commit) = 0;
  virtual void loot_force_reconnect() = 0;
  virtual bool loot_has_combat() const = 0;
  virtual bool loot_has_world_mobs() const = 0;
  virtual const nlohmann::json &loot_current_combat_stats() const = 0;
  virtual std::int64_t loot_monotonic_ticks() const = 0;
  virtual void loot_publish_snapshot(const Character &character,
                                     const nlohmann::json *prepared_stats) = 0;
};

class LootPlayerSession final {
public:
  LootPlayerSession(LootFeature &feature, LootSessionContext &context);

  void create_tutorial(std::int64_t character_id, const Handoff &entry);
  // Called only after the combat owner attaches its shared population. Cache loading stays
  // before the absent-data guard, including a server where corpse loot is disabled.
  void attach_world_loot();
  void reset();
  bool has_session() const noexcept;
  bool handles(std::uint16_t opcode) const;
  bool handle_shared(FrameContext &frame);
  std::vector<GameplayPacket> receive(const nlohmann::json &request, const WorldPosition &position,
                                      std::int64_t now);
  std::vector<GameplayPacket> advance(std::int64_t now);
  std::optional<std::int64_t> next_action_tick() const;
  void on_world_progress_committed(const WorldProgress &progress);

  // Monsters whose corpses the killer may butcher; applied to every loot session this player
  // session creates, including later ones.
  void set_butchering(std::function<bool(int npc_cms_id)> butcherable);
  std::optional<ButcherableCorpse> butcherable_corpse(std::uint64_t unit_id,
                                                      std::int64_t now) const;
  std::vector<GameplayPacket> corpse_butchered(std::uint64_t unit_id, std::int64_t now);

private:
  LootFeature &feature_;
  LootSessionContext &context_;
  std::unique_ptr<LootSession> session_;
  WorldProgress world_progress_;
  std::function<bool(int npc_cms_id)> butcherable_;
};

} // namespace chronicles::loot
