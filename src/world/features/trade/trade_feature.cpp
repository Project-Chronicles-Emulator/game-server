#include "world/features/trade/trade_feature.hpp"

#include "common/catalog_fields.hpp"
#include "data/catalog_requirements.hpp"
#include "data/client_catalog.hpp"

#include <spdlog/spdlog.h>

#include <utility>

namespace chronicles::trade {
namespace {

constexpr std::uint16_t kWorldCharTradeVoucherNotification = 142;

std::shared_ptr<const std::set<int>> load_managers(ClientCatalog *catalog) {
  auto managers = std::make_shared<std::set<int>>();
  if (!catalog || !catalog->has_table("Npc")) {
    return managers;
  }

  for (const auto &row : catalog->rows("Npc")) {
    if (catalog_fields::text(row, "category") == "TradeManager") {
      managers->insert(catalog_fields::required_identifier(row, "id"));
    }
  }
  return managers;
}

std::shared_ptr<const TradeCatalog> load_catalog(ClientCatalog *catalog,
                                                 const std::filesystem::path &root) {
  if (!catalog || !has_catalog_tables(*catalog, TradeCatalog::required_tables(), "Trade runs")) {
    return {};
  }

  auto trade = TradeCatalog::load(*catalog, root);
  std::size_t placed = 0;
  for (const auto &[post_id, post] : trade->posts()) {
    placed += post.placed ? 1 : 0;
  }
  spdlog::info("Trade catalog loaded: {} trading posts with wagon placements", placed);
  return trade;
}

} // namespace

TradeFeature::TradeFeature(ClientCatalog *catalog, std::shared_ptr<const ShopCatalog> items)
    : TradeFeature({}, load_managers(catalog), std::move(items)) {
}

TradeFeature::TradeFeature(std::shared_ptr<const TradeCatalog> catalog,
                           std::shared_ptr<const std::set<int>> managers,
                           std::shared_ptr<const ShopCatalog> items)
    : catalog_(std::move(catalog)), managers_(std::move(managers)), items_(std::move(items)) {
}

void TradeFeature::load(ClientCatalog *catalog, const std::filesystem::path &root) {
  catalog_ = load_catalog(catalog, root);
}

const std::shared_ptr<const TradeCatalog> &TradeFeature::catalog() const noexcept {
  return catalog_;
}

std::unique_ptr<TradeSession>
TradeFeature::create_session(const companion::CompanionFeature &companions,
                             std::unique_ptr<ProgressAccess> progress, std::uint64_t connection_id,
                             std::int64_t character_id) const {
  if (!catalog_) {
    return {};
  }

  TradeServices services{companions.world(), companions.catalog(), catalog_, items_, managers_};
  return std::make_unique<TradeSession>(std::move(services), std::move(progress), connection_id,
                                        character_id);
}

bool TradeFeature::handle_dialogue(TradeSession *session, const WorldGameplayRequest &request,
                                   const std::optional<WorldPosition> &position,
                                   TradeSessionContext &context,
                                   companion::SessionResult &result) const {
  if (!session || !position || request.opcode != kDialogueStartRequest) {
    return false;
  }

  const auto &message = request.message;
  const auto target = message.contains("target_unit_id") && message.at("target_unit_id").is_string()
                          ? parse_unit_id_text(message.at("target_unit_id").get<std::string>())
                          : std::nullopt;
  const auto *unit = target ? context.trade_interaction_npc(*target) : nullptr;
  if (!unit || !session->is_trade_manager(unit->npc_template_id)) {
    return false;
  }

  const WorldPosition npc_position{unit->map_id, unit->position.x_mm, unit->position.y_mm,
                                   unit->position.z_mm, 0.0F};
  companion::append_session_result(result,
                                   session->start_dialogue(*target, npc_position, *position));
  return true;
}

bool TradeFeature::handle_request(TradeSession *session, const WorldGameplayRequest &request,
                                  const std::optional<WorldPosition> &position,
                                  TradeSessionContext &context,
                                  companion::SessionResult &result) const {
  if (!session || !position || !session->handles(request.opcode, request.message)) {
    return false;
  }

  companion::append_session_result(result, session->receive(request.opcode, request.message,
                                                            request.sequence, *position,
                                                            context.trade_epoch_ticks()));
  return true;
}

void TradeBootstrap::configure(std::optional<WorldProgress> &non_tutorial_progress) {
  if (non_tutorial_progress) {
    vouchers_ = std::move(non_tutorial_progress->trade_vouchers);
  } else {
    vouchers_.clear();
  }
}

std::optional<GameplayPacket> TradeBootstrap::snapshot() const {
  auto vouchers = nlohmann::json::array();
  for (const auto &[voucher_id, record] : vouchers_) {
    if (!record.redeemed) {
      vouchers.push_back({{"voucher_id", std::to_string(voucher_id)},
                          {"good_cms_id", record.good_cms_id},
                          {"created_epochtime", std::to_string(record.created_epoch_ticks)},
                          {"expiration_epochtime", std::to_string(record.expiration_epoch_ticks)}});
    }
  }
  if (vouchers.empty()) {
    return std::nullopt;
  }
  return GameplayPacket{kWorldCharTradeVoucherNotification, {{"vouchers", std::move(vouchers)}}};
}

} // namespace chronicles::trade
