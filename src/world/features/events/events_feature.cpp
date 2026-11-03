#include "world/features/events/events_feature.hpp"

#include "data/catalog_requirements.hpp"
#include "data/client_catalog.hpp"
#include "world/features/events/events_session.hpp"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/post.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <exception>
#include <utility>

namespace chronicles::events {
namespace {

namespace asio = boost::asio;

// Original native-world raid tick interval; the existing party strand owns the timer.
constexpr auto kRaidTickInterval = std::chrono::seconds(1);

} // namespace

EventsDefinitions EventsFeature::load_catalogs(ClientCatalog *catalog) {
  EventsDefinitions definitions;
  if (catalog &&
      has_catalog_tables(*catalog, ranking::RankingCatalog::required_tables(), "Rankings")) {
    definitions.rankings = ranking::RankingCatalog::load(*catalog);
    spdlog::info("Ranking catalog loaded: {} leaderboards", definitions.rankings->size());
  }

  if (catalog &&
      has_catalog_tables(*catalog, raid::RaidCatalog::required_tables(), "Raid channels")) {
    auto raids = raid::RaidCatalog::load(*catalog);
    spdlog::info("Raid channel catalog loaded: {} raid maps, auto-enter {} ms", raids->size(),
                 raids->auto_enter_delay().count());
    definitions.raids =
        std::make_shared<raid::RaidMatchmaker>(std::move(raids), raid::RaidMatchmakerConfig{});
  }
  return definitions;
}

EventsFeature::EventsFeature(asio::any_io_executor timer_executor, ClientCatalog *catalog,
                             std::shared_ptr<party::PartyWorld> party_world)
    : EventsFeature(std::move(timer_executor), load_catalogs(catalog), std::move(party_world)) {
}

EventsFeature::EventsFeature(asio::any_io_executor timer_executor, EventsDefinitions definitions,
                             std::shared_ptr<party::PartyWorld> party_world)
    : ranking_catalog_(std::move(definitions.rankings)),
      raid_matchmaker_(std::move(definitions.raids)), timer_executor_(std::move(timer_executor)),
      raid_timer_(timer_executor_), channel_event_data_(std::move(definitions.channel_events)),
      party_world_(std::move(party_world)) {
}

std::unique_ptr<EventsSession> EventsFeature::create_session(EventsSessionContext &context) {
  return std::make_unique<EventsSession>(*this, context);
}

void EventsFeature::load_channel_event_data(const std::filesystem::path &catalog_root) {
  if (catalog_root.empty()) {
    return;
  }
  const auto path = catalog_root.parent_path() / "channel-events" / "definitions.json";
  if (!std::filesystem::exists(path)) {
    spdlog::info("Channel event definitions not found at {}", path.string());
    return;
  }
  channel_event_data_ = channel_event::ChannelEventData::load(path);
  spdlog::info("Channel event definitions loaded: maps={} tribute_altars={}",
               channel_event_data_->binding_count(), channel_event_data_->altar_count());
}

void EventsFeature::set_ranking_repository(std::shared_ptr<RankingRepository> repository) {
  ranking_repository_ = std::move(repository);
}

void EventsFeature::set_raid_delivery(std::function<void(const raid::RaidUpdate &)> delivery) {
  raid_delivery_ = std::move(delivery);
}

bool EventsFeature::has_raid_service() const noexcept {
  return raid_matchmaker_ != nullptr;
}

bool EventsFeature::is_raid_map(int map_id) const {
  return raid_matchmaker_ && raid_matchmaker_->catalog().find(map_id);
}

std::optional<int> EventsFeature::raid_parent_map(int map_id) const {
  if (raid_matchmaker_) {
    if (const auto *map = raid_matchmaker_->catalog().find(map_id)) {
      return map->parent_map_id;
    }
  }
  return std::nullopt;
}

std::optional<std::uint64_t> EventsFeature::raid_instance_owner(std::int64_t character_id,
                                                                int map_id) const {
  if (!is_raid_map(map_id)) {
    return std::nullopt;
  }
  const auto membership = raid_matchmaker_->membership(character_id);
  if (membership.state != raid::RaidState::matched || membership.channel_id == 0 ||
      membership.map_id != map_id) {
    return std::nullopt;
  }
  return membership.channel_id;
}

void EventsFeature::deliver_raid_update(const raid::RaidUpdate &update) const {
  if (raid_delivery_) {
    raid_delivery_(update);
  }
}

void EventsFeature::schedule_raid_tick() {
  if (!raid_matchmaker_) {
    return;
  }

  std::weak_ptr<EventsFeature> weak_feature = shared_from_this();
  asio::post(timer_executor_, [weak_feature] {
    const auto feature = weak_feature.lock();
    if (!feature || feature->raid_tick_armed_ || !feature->raid_matchmaker_->active()) {
      return;
    }

    feature->raid_tick_armed_ = true;
    feature->raid_timer_.expires_after(kRaidTickInterval);
    feature->raid_timer_.async_wait(asio::bind_executor(
        feature->timer_executor_, [weak_feature](boost::system::error_code error) {
          const auto ticking_feature = weak_feature.lock();
          if (!ticking_feature) {
            return;
          }

          ticking_feature->raid_tick_armed_ = false;
          if (error) {
            return;
          }

          try {
            ticking_feature->deliver_raid_update(
                ticking_feature->raid_matchmaker_->tick(raid::Clock::now()));
          } catch (const std::exception &failure) {
            spdlog::error("Raid channel tick failed: {}", failure.what());
          }
          ticking_feature->schedule_raid_tick();
        }));
  });
}

} // namespace chronicles::events
