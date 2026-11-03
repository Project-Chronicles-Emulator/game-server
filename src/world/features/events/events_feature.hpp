#pragma once

#include "world/features/events/channel_event.hpp"
#include "world/features/events/raid_channel.hpp"
#include "world/features/events/ranking.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/steady_timer.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

namespace chronicles {

class ClientCatalog;

namespace party {
class PartyWorld;
}

namespace events {

class EventsSession;
class EventsSessionContext;

// Explicit startup inputs also permit synthetic tests without constructing a world server.
struct EventsDefinitions {
  std::shared_ptr<const ranking::RankingCatalog> rankings;
  std::shared_ptr<raid::RaidMatchmaker> raids;
  std::shared_ptr<const channel_event::ChannelEventData> channel_events;
};

// One per server: immutable definitions, shared event state, ranking storage and raid tick.
// The composition supplies the existing party strand, never a new I/O thread or timer strand.
class EventsFeature final : public std::enable_shared_from_this<EventsFeature> {
public:
  EventsFeature(boost::asio::any_io_executor timer_executor, ClientCatalog *catalog,
                std::shared_ptr<party::PartyWorld> party_world);

  EventsFeature(boost::asio::any_io_executor timer_executor, EventsDefinitions definitions,
                std::shared_ptr<party::PartyWorld> party_world);

  std::unique_ptr<EventsSession> create_session(EventsSessionContext &context);

  // Called at the original end-of-WorldState-construction position, after other data loads.
  // A missing optional file logs and disables tribute; malformed present data still throws.
  void load_channel_event_data(const std::filesystem::path &catalog_root);

  // Installed before accepting sessions, at the existing repository/delivery wiring points.
  // The delivery closure captures only weak world ownership to avoid a world/feature cycle.
  void set_ranking_repository(std::shared_ptr<RankingRepository> repository);
  void set_raid_delivery(std::function<void(const raid::RaidUpdate &)> delivery);

  bool has_raid_service() const noexcept;
  bool is_raid_map(int map_id) const;
  std::optional<int> raid_parent_map(int map_id) const;
  std::optional<std::uint64_t> raid_instance_owner(std::int64_t character_id, int map_id) const;

  void deliver_raid_update(const raid::RaidUpdate &update) const;
  void schedule_raid_tick();

private:
  friend class EventsSession;

  static EventsDefinitions load_catalogs(ClientCatalog *catalog);

  const std::shared_ptr<const ranking::RankingCatalog> ranking_catalog_;
  const std::shared_ptr<raid::RaidMatchmaker> raid_matchmaker_;
  boost::asio::any_io_executor timer_executor_;
  boost::asio::steady_timer raid_timer_;
  bool raid_tick_armed_{};
  std::shared_ptr<const channel_event::ChannelEventData> channel_event_data_;
  channel_event::ChannelEventBoard channel_events_;
  std::shared_ptr<party::PartyWorld> party_world_;
  std::shared_ptr<RankingRepository> ranking_repository_;
  std::function<void(const raid::RaidUpdate &)> raid_delivery_;
};

} // namespace events
} // namespace chronicles
