#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace chronicles {

class ClientCatalog;
class UnitRegistry;

// Immutable catalog index answering "which interactions and shops does an NPC template offer".
//
// Catalog contract (docs: out/agent-team-reports/merchant-validation/npc-registry-audit.md):
//   Npc.interactionGroupId == Interaction.groupId                (an NPC template has one group)
//   Interaction.playerTaskCase == "Shop", playerTask '"shopId":N' -> Shop.id
// This module knows catalog identities only. It has no notion of a spawned NPC, a unit id, a map
// or instance, a position or a range; those are separate server state.
class NpcInteractionIndex {
public:
  // Builds the index from raw catalog rows. Throws std::invalid_argument naming the table, the
  // zero-based row index and the offending field for any malformed row.
  static NpcInteractionIndex build(const std::vector<nlohmann::json> &npc_rows,
                                   const std::vector<nlohmann::json> &interaction_rows,
                                   const std::vector<nlohmann::json> &shop_rows);

  // Reads the Npc, Interaction and Shop tables through the existing verified catalog loader.
  static NpcInteractionIndex load(ClientCatalog &catalog);

  bool npc_offers_interaction(std::int64_t npc_id, std::int64_t interaction_id) const;

  // True when any interaction of the NPC template is a Shop interaction for `shop_id`.
  bool npc_offers_shop(std::int64_t npc_id, std::int64_t shop_id) const;

  // Shop id of a Shop interaction; nullopt for unknown or non-shop interactions.
  std::optional<std::int64_t> shop_for_interaction(std::int64_t interaction_id) const;

  // Sorted interaction ids offered by the NPC template; empty for an unknown template or one
  // whose group has no Interaction rows.
  const std::vector<std::int64_t> &interactions_for_npc(std::int64_t npc_id) const;

  std::size_t npc_count() const;
  std::size_t interaction_count() const;

private:
  NpcInteractionIndex() = default;

  std::unordered_map<std::int64_t, std::vector<std::int64_t>> npc_interactions_;
  std::unordered_map<std::int64_t, std::optional<std::int64_t>> interaction_shop_;
};

// The shop a session currently has open. Every field is derived from server state at open time:
// `shop_id` comes from the catalog interaction, `unit_id`, `map_id` and `instance_key` from the
// server's own announcement of the unit. Nothing is copied from the client request.
struct OpenedShop {
  std::int64_t shop_id{};
  std::int64_t interaction_id{};
  std::uint64_t unit_id{};
  int map_id{};
  std::string instance_key;
};

enum class ShopOpenResult {
  Opened,
  UnknownUnit,          // not announced by the server on this map/instance (or despawned)
  InteractionNotOffered, // the unit's NPC template does not offer this interaction
  NotAShopInteraction,   // the interaction offered is not a Shop interaction
};

// Per-session eligibility record for later buy/sell requests. It is confined to the session
// worker (no locking, no global state) and stores no time, range or expiry: none is established.
// Candidate, not confirmed: PC2S_DialogueEndReq (20066) / PS2C_DialogueEndNoti (162) exist, but no
// capture ties them to closing a shop, so no close message is mapped here; the owner calls
// close() from whatever event is later proven.
class OpenedShopState {
public:
  // Opens `interaction_id` on the unit if, and only if, the server announced the unit on this
  // map and instance (UnitRegistry), its NPC template offers the interaction (index), and the
  // interaction is a Shop interaction. On success any previously opened shop is replaced. On any
  // failure the existing state is left unchanged.
  ShopOpenResult open(const NpcInteractionIndex &index, const UnitRegistry &registry,
                      std::uint64_t unit_id, std::int64_t interaction_id, int map_id,
                      std::string_view instance_key);

  // True only when exactly this shop is open on exactly this unit, map and instance.
  bool permits(std::int64_t shop_id, std::uint64_t unit_id, int map_id,
               std::string_view instance_key) const;

  // Clears the state when the session moved to a different map or instance.
  void on_map_or_instance_change(int map_id, std::string_view instance_key);

  // Clears the state when the opened shop's unit despawned.
  void on_unit_despawn(std::uint64_t unit_id);

  // Explicit close, and disconnect/teardown.
  void clear();

  const std::optional<OpenedShop> &current() const;

private:
  std::optional<OpenedShop> opened_;
};

} // namespace chronicles
