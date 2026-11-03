#include "world/features/guild/guild_feature.hpp"

#include <spdlog/spdlog.h>

#include <utility>

namespace chronicles::guild {

GuildFeature::GuildFeature(std::shared_ptr<GuildService> service,
                           std::function<void(const guild_world::GuildWorldUpdate &)> delivery)
    : service_(std::move(service)),
      world_(service_ ? std::make_shared<GuildWorld>(service_) : nullptr),
      delivery_(std::move(delivery)) {
}

void GuildFeature::deliver_update(const guild_world::GuildWorldUpdate &update) const {
  if (update.empty()) {
    return;
  }

  if (delivery_) {
    delivery_(update);
    return;
  }

  if (!dropped_delivery_reported_.exchange(true)) {
    spdlog::error("Guild update dropped because no delivery is installed");
  }
}

void GuildFeature::subscribe() {
  if (!service_) {
    return;
  }

  std::weak_ptr<GuildFeature> weak_feature = weak_from_this();
  service_->subscribe([weak_feature](const std::vector<GuildEvent> &events) {
    const auto feature = weak_feature.lock();
    if (!feature) {
      return;
    }

    feature->deliver_update(feature->world_->translate(events));
  });
}

std::unique_ptr<GuildSession> GuildFeature::create_session(GuildSessionContext &context) {
  return std::make_unique<GuildSession>(*this, context);
}

const std::shared_ptr<GuildService> &GuildFeature::service() const noexcept {
  return service_;
}

const std::shared_ptr<GuildWorld> &GuildFeature::world() const noexcept {
  return world_;
}

GuildSession::GuildSession(GuildFeature &feature, GuildSessionContext &context)
    : feature_(feature), context_(context) {
}

std::optional<GuildActor> GuildSession::actor() const {
  auto actor = context_.guild_identity();
  if (!actor) {
    return std::nullopt;
  }

  if (const auto company = context_.guild_company_status(actor->char_id)) {
    actor->company_type = company->record.company_type;
    actor->company_position = company->position_level;
  }
  return actor;
}

guild_world::GuildWorldUpdate GuildSession::on_enter_world(bool entered_world) {
  if (!feature_.world() || !entered_world || attached_ || context_.guild_closed()) {
    return {};
  }
  const auto current_actor = actor();
  if (!current_actor) {
    return {};
  }

  attached_ = true;
  auto update = feature_.world()->attach(*current_actor, context_.guild_connection_id());
  if (context_.guild_closed()) {
    feature_.world()->detach(current_actor->char_id, context_.guild_connection_id());
  }
  return update;
}

guild_world::GuildWorldUpdate GuildSession::receive(const WorldGameplayRequest &request) {
  return feature_.world()->receive(context_.guild_requester_id(), request.opcode, request.message,
                                   request.sequence);
}

void GuildSession::refresh_member() {
  const auto current_actor = actor();
  if (feature_.service() && current_actor) {
    feature_.service()->refresh_member(*current_actor);
  }
}

void GuildSession::record_activity_experience(std::int64_t character_id, std::int64_t experience) {
  if (feature_.service()) {
    feature_.service()->record_character_experience(character_id, experience);
  }
}

void GuildSession::on_leave_world() {
  if (attached_ && feature_.world()) {
    feature_.world()->detach(context_.guild_requester_id(), context_.guild_connection_id());
  }
}

} // namespace chronicles::guild
