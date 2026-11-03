#include "world/features/housing/housing_feature.hpp"

#include <boost/asio/post.hpp>
#include <spdlog/spdlog.h>

#include <utility>

namespace chronicles::housing {

HousingFeature::HousingFeature(boost::asio::any_io_executor io_executor,
                               boost::asio::any_io_executor worker_executor,
                               std::chrono::steady_clock::duration expiry_interval)
    : expiry_timer_(std::in_place, std::move(io_executor)),
      worker_executor_(std::move(worker_executor)), expiry_interval_(expiry_interval) {
}

void HousingFeature::set_hub(std::shared_ptr<HousingHub> hub) {
  hub_ = std::move(hub);
}

bool HousingFeature::enabled() const noexcept {
  return hub_ != nullptr;
}

std::optional<WorldPacket>
HousingFeature::late_viewer_pose(const nlohmann::json &unit_add_message) const {
  if (!hub_) {
    return std::nullopt;
  }
  return hub_->late_viewer_pose(unit_add_message);
}

std::unique_ptr<HousingSession>
HousingFeature::create_session(::chronicles::ProgressAccess &progress,
                               HousingSessionContext &context) {
  return std::make_unique<HousingSession>(*this, progress, context);
}

void HousingFeature::schedule_expiry(std::weak_ptr<HousingExpiryOwner> weak_owner) {
  if (!hub_ || !expiry_timer_) {
    return;
  }
  expiry_timer_->expires_after(expiry_interval_);
  expiry_timer_->async_wait([this, weak_owner](boost::system::error_code error) {
    const auto owner = weak_owner.lock();
    if (error || !owner || !owner->housing_listener_open()) {
      return;
    }
    boost::asio::post(worker_executor_, [this, weak_owner] {
      if (const auto owner = weak_owner.lock()) {
        try {
          hub_->expire_due();
        } catch (const std::exception &failure) {
          spdlog::error("Housing expiry sweep failed: {}", failure.what());
        }
      }
    });
    schedule_expiry(weak_owner);
  });
}

void HousingFeature::stop() {
  if (expiry_timer_) {
    expiry_timer_->cancel();
  }
}

void HousingFeature::on_listener_destroyed() noexcept {
  expiry_timer_.reset();
}

} // namespace chronicles::housing
