#pragma once

#include "world/core/frame_context.hpp"
#include "world/features/company/company.hpp"
#include "world/features/guild/guild_world.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

namespace chronicles::guild {

class GuildSession;
class GuildSessionContext;
class GuildReplicationContext;

class GuildFeature final : public std::enable_shared_from_this<GuildFeature> {
public:
  GuildFeature(std::shared_ptr<GuildService> service,
               std::function<void(const guild_world::GuildWorldUpdate &)> delivery);

  // Subscribes to guild service events; call before accepting players so no event is missed.
  void subscribe();

  // Hands one translated update to the delivery given at construction. Without a delivery a
  // non-empty update is dropped and reported once.
  void deliver_update(const guild_world::GuildWorldUpdate &update) const;

  std::unique_ptr<GuildSession> create_session(GuildSessionContext &context);
  const std::shared_ptr<GuildService> &service() const noexcept;
  const std::shared_ptr<GuildWorld> &world() const noexcept;

  void replicate_update(const guild_world::GuildWorldUpdate &update,
                        GuildReplicationContext &context) const;

private:
  std::shared_ptr<GuildService> service_;
  std::shared_ptr<GuildWorld> world_;
  std::function<void(const guild_world::GuildWorldUpdate &)> delivery_;
  // Subscriber callbacks may run on any thread that publishes a guild event.
  mutable std::atomic<bool> dropped_delivery_reported_{};
};

class GuildSessionContext {
public:
  virtual ~GuildSessionContext() = default;

  // Authenticated character-row fields only; the feature enriches company membership.
  virtual std::optional<GuildActor> guild_identity() const = 0;
  virtual std::optional<CompanyStatus> guild_company_status(std::int64_t character_id) const = 0;
  virtual std::uint64_t guild_connection_id() const = 0;
  virtual std::int64_t guild_requester_id() const = 0;
  virtual bool guild_closed() const = 0;
};

class GuildSession final {
public:
  GuildSession(GuildFeature &feature, GuildSessionContext &context);

  std::optional<GuildActor> actor() const;
  guild_world::GuildWorldUpdate on_enter_world(bool entered_world);
  guild_world::GuildWorldUpdate receive(const WorldGameplayRequest &request);
  void refresh_member();
  void record_activity_experience(std::int64_t character_id, std::int64_t experience);
  void on_leave_world();

private:
  GuildFeature &feature_;
  GuildSessionContext &context_;
  std::atomic<bool> attached_{};
};

} // namespace chronicles::guild
