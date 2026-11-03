#pragma once

// ItemsUpdateNoti (169) describing a committed progress transition: the stackable stacks whose
// quantity changed and the Gold/Hersander Token balance changes. Used when another service
// (guild housing rent, pioneer permits) changed a character's progress outside the session's
// own handlers, so the client's inventory and wallet stay in step.

#include "common/currency.hpp"
#include "persistence/character_state_types.hpp"
#include "protocol/gameplay_packet.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace chronicles {

// Money-type wire names used in ItemsUpdateNoti money_changes entries.
inline constexpr char kGoldMoneyType[] = "EX5MoneyType_Gold";
inline constexpr char kTokenMoneyType[] = "EX5MoneyType_HersanderToken";

// Wire form of one MoneyChange entry in ItemsUpdateNoti.money_changes. Gold (the protobuf
// default) is omitted unless `omit_type_on_wire` is false.
nlohmann::json money_change_json(const MoneyChange &change);

// Wire form of one HousingItemCount (ItemsUpdateNoti.housing_items and
// WorldCharHousingItemsNoti). The stock client stores `quantity` as the copies still in
// housing storage and allows a placement while that value is above zero; it shows the same
// value in the decor list (AAC_Housing_ApplyHousingItemCount, AAC_Housing_CanPlaceSelectedItem).
// `quantity` is therefore the unplaced copies, not every owned copy.
nlohmann::json housing_item_count_json(int item_cms_id, const HousingItemRecord &record);

// `reason` is the ItemsUpdateNoti reason field name, for example "housing" or
// "company_pioneer_permit". nullopt when neither stacks nor balances changed.
std::optional<GameplayPacket> progress_change_packet(const WorldProgress &before,
                                                     const WorldProgress &after,
                                                     const std::string &reason);

} // namespace chronicles
