#include "world/features/nuia/nuia_feature.hpp"

#include <spdlog/spdlog.h>

#include <utility>

namespace chronicles::nuia {

NuiaFeature::NuiaFeature(std::shared_ptr<const NuiStatueData> statues,
                         std::optional<NuiaTears> tears)
    : tears_(std::move(tears)), statues_(std::move(statues)) {
}

void NuiaFeature::load(const std::filesystem::path &catalog_root) {
  if (catalog_root.empty()) {
    return;
  }

  const auto statue_path = catalog_root.parent_path() / "nui-statues" / "definitions.json";
  if (std::filesystem::exists(statue_path)) {
    statues_ = NuiStatueData::load(statue_path);
  } else {
    spdlog::info("Nui Statue definitions not found at {}", statue_path.string());
  }
  const auto path = catalog_root.parent_path() / "nuia-tears" / "definitions.json";
  if (std::filesystem::exists(path)) {
    tears_.emplace(NuiaTears::load(path));
  } else {
    spdlog::info("Nuia Tear definitions not found at {}", path.string());
  }
}

bool NuiaFeature::has_statues() const noexcept {
  return statues_ != nullptr;
}

bool NuiaFeature::has_tears() const noexcept {
  return tears_.has_value();
}

bool NuiaFeature::has_data() const noexcept {
  return has_tears() || has_statues();
}

std::vector<GameplayPacket> NuiaFeature::apply_revival_supplies(int task_id,
                                                                WorldProgress &progress,
                                                                const Character &character) const {
  return statues_->apply_revival_supplies(task_id, progress, &character);
}

std::unique_ptr<NuiaSession> NuiaFeature::create_session(ProgressAccess &progress,
                                                         NuiaSessionContext &context) {
  return std::make_unique<NuiaSession>(*this, progress, context);
}

} // namespace chronicles::nuia
