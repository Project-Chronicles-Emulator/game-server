#include "world/features/quests/quests_feature.hpp"

#include <spdlog/spdlog.h>

#include <utility>

namespace chronicles::quests {

void QuestsFeature::load_catalog(const std::filesystem::path &catalog_root) {
  if (catalog_root.empty()) {
    return;
  }
  const auto path = catalog_root.parent_path() / "quests" / "catalog.json";
  if (!std::filesystem::exists(path)) {
    spdlog::info("Shared-world quest catalog not found at {}", path.string());
    return;
  }
  catalog_ = QuestCatalog::load(path);
  spdlog::info("Shared-world quest catalog loaded: quests={} fully_supported={}", catalog_->size(),
               catalog_->fully_supported_count());
}

const std::shared_ptr<const QuestCatalog> &QuestsFeature::catalog() const noexcept {
  return catalog_;
}

std::unique_ptr<QuestsSession> QuestsFeature::create_session(
    character::CharacterProgressStore &store, QuestsSessionContext &context,
    std::shared_ptr<const Progression> progression, SessionEventBus &events) {
  return std::make_unique<QuestsSession>(*this, store, context, std::move(progression), events);
}

} // namespace chronicles::quests
