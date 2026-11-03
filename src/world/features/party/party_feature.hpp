#pragma once

#include "persistence/character_repository.hpp"
#include "world/core/frame_context.hpp"
#include "world/features/party/party_world.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

namespace chronicles::party {

class PartySession;
class PartySessionContext;
class PartyReplicationContext;

// The existing membership directory and timer retain one shared strand per world.
class PartyFeature final : public std::enable_shared_from_this<PartyFeature> {
public:
  PartyFeature(boost::asio::io_context &io_context, ClientCatalog *catalog);
  PartyFeature(boost::asio::io_context &io_context, PartyWorldConfig config);

  std::unique_ptr<PartySession> create_session(PartySessionContext &context);
  const std::shared_ptr<PartyWorld> &world() const noexcept;
  boost::asio::any_io_executor executor() const;

  // Installed before accepting connections. The composition captures weak world ownership.
  void set_delivery(std::function<void(const PartyUpdate &)> delivery);
  void schedule_tick();

  // Hands one tick's update to the installed delivery. Without a delivery a non-empty update
  // is dropped and reported once, so a composition that forgot set_delivery shows up in the log.
  // Call on executor(): the drop flag is confined to the feature's strand and not synchronized.
  void deliver_tick_update(const PartyUpdate &update);

  void replicate_update(const PartyUpdate &update, PartyReplicationContext &context) const;

private:
  std::shared_ptr<PartyWorld> world_;
  boost::asio::strand<boost::asio::io_context::executor_type> strand_;
  boost::asio::steady_timer timer_;
  bool tick_armed_{};
  bool dropped_delivery_reported_{};
  std::function<void(const PartyUpdate &)> delivery_;
};

class PartySessionContext {
public:
  virtual ~PartySessionContext() = default;

  virtual std::uint64_t party_connection_id() const = 0;
  virtual bool party_closed() const = 0;
  virtual bool party_has_tutorial() const = 0;
  virtual std::optional<nlohmann::json> party_character_stats(const Character &character) const = 0;
  virtual const nlohmann::json *party_combat_stats() const = 0;
  virtual const WorldProgress *party_tutorial_progress() const = 0;
  virtual TimePoint party_now() const = 0;
};

// Character/connection identity is established before attached_'s atomic publication.
// Level and unlock changes stay worker-owned; I/O close reads only the stable character ID.
class PartySession final {
public:
  PartySession(PartyFeature &feature, PartySessionContext &context);

  void select_character(const Character &character,
                        const std::optional<WorldProgress> &stored_progress);
  const MemberPresence *identity() const noexcept;
  void update_level(int level);
  void update_unlock(const WorldProgress &progress);
  void on_tutorial_graduation(const WorldProgress &progress);
  // Kill credit and loot sharing stay inside one instance copy (InstanceCopy::channel_key; 0 on
  // shared maps). The private tutorial keeps its own key.
  void set_instance_channel(std::uint64_t channel_key);
  void set_quest_ledger(std::shared_ptr<quests::QuestIntentLedger> ledger);
  MemberPresence presence(const WorldPosition &position) const;
  PartyUpdate on_enter_or_refresh(const std::optional<WorldPosition> &position, bool entered_world);
  PartyUpdate receive(const WorldGameplayRequest &request);
  bool attached() const noexcept;
  PartyUpdate on_leave_world();
  PartyUpdate on_leave_world_staged(
      const std::function<void(const PartyUpdate &)> &before_commit);

private:
  PartyFeature &feature_;
  PartySessionContext &context_;
  std::optional<MemberPresence> identity_;
  std::atomic<bool> attached_{};
};

} // namespace chronicles::party
