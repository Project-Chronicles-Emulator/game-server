#pragma once

#include "world/core/world_gameplay_request.hpp"
#include "world/features/companion/companion_session.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace chronicles::companion {

void append_session_result(SessionResult &target, SessionResult source);

// These synchronous capabilities borrow the containing connection. Trade already depends
// on Companion; its lifecycle commands remain explicit so this owner has no reverse edge.
class CompanionSessionContext {
public:
  virtual ~CompanionSessionContext() = default;

  virtual bool companion_has_handoff() const = 0;
  virtual bool companion_is_game_master() const = 0;
  virtual std::uint64_t companion_connection_id() const = 0;
  virtual std::int64_t companion_character_id() const = 0;
  virtual std::int64_t companion_epoch_ticks() const = 0;
  // Instance copy channel of the character (InstanceCopy::channel_key); 0 on shared maps.
  virtual std::uint64_t companion_channel_key() const {
    return 0;
  }
  // Native's map/copy generation; separate from the companion channel key.
  virtual CompanionViewToken companion_view_token() const {
    return {};
  }
  // A partially completed entry must close through the retained session owner.
  virtual void companion_entry_failed() noexcept {
  }
  virtual bool companion_allow_gm_request() = 0;
  virtual bool companion_allow_player_car() = 0;
  virtual void companion_reply(const std::string &message) = 0;

  virtual void companion_trade_commands(const std::vector<CompanionGmCommand> &commands,
                                        SessionResult &result) = 0;
  virtual void companion_enter_trade(const WorldPosition &position, SessionResult &result) = 0;
  virtual void companion_trade_dismounted(SessionResult &result) = 0;
  // RideUnitReq was handled; a restored trade run may announce itself once its owner drives.
  virtual void companion_trade_boarded(SessionResult &result) = 0;
  // Called under the companion-world lock before detach; no database or executor work.
  virtual void companion_capture_wagons(const std::vector<VehicleSnapshot> &) {
  }
  virtual bool companion_settle_wagons(const std::vector<VehicleSnapshot> &vehicles) = 0;
  virtual void companion_deliver_update(const CompanionUpdate &update) = 0;
};

// CompanionSession already owns the player's actual mount/mate state. This feature owns
// catalog/world startup and the former native request/lifecycle policy, without another shell.
class CompanionFeature final {
public:
  CompanionFeature(ClientCatalog *catalog, std::shared_ptr<const ShopCatalog> items,
                   std::shared_ptr<CompanionDetachBudget> detach_budget = {});
  CompanionFeature(std::shared_ptr<const CompanionCatalog> catalog,
                   std::shared_ptr<const ShopCatalog> items,
                   std::shared_ptr<CompanionDetachBudget> detach_budget = {});

  bool enabled() const noexcept;
  const std::shared_ptr<const CompanionCatalog> &catalog() const noexcept;
  const std::shared_ptr<CompanionWorld> &world() const noexcept;
  std::unique_ptr<CompanionSession> create_session(std::unique_ptr<ProgressAccess> progress,
                                                   std::uint64_t connection_id,
                                                   std::int64_t character_id) const;

  bool handle_command(CompanionSession &session, const WorldGameplayRequest &request,
                      const std::optional<WorldPosition> &position,
                      CompanionSessionContext &context, SessionResult &result) const;
  bool handle_request(CompanionSession &session, const WorldGameplayRequest &request,
                      CompanionSessionContext &context, SessionResult &result) const;
  void enter_world(CompanionSession &session, const WorldPosition &position,
                   CompanionSessionContext &context, SessionResult &result) const;
  bool leave_world(CompanionSession &session, CompanionSessionContext &context) const;

  // Player chat commands open to every character (`!mount`, `!dismount`). They run on the
  // session strand like a request; the caller delivers the result and the reply line.
  PlayerCompanionOutcome player_mount(CompanionSession &session,
                                      CompanionSessionContext &context) const;
  PlayerCompanionOutcome player_dismount(CompanionSession &session,
                                         CompanionSessionContext &context) const;

private:
  const std::shared_ptr<const CompanionCatalog> catalog_;
  const std::shared_ptr<CompanionWorld> world_;
  const std::shared_ptr<const ShopCatalog> items_;
};

} // namespace chronicles::companion
