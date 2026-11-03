#pragma once

#include "protocol/gameplay_packet.hpp"
#include "persistence/character_repository.hpp"
#include "world/features/mail/mail_types.hpp"

#include <nlohmann/json.hpp>

namespace chronicles {

class ShopCatalog;

struct MailClaimPlan {
  WorldProgress before;
  WorldProgress after;
  std::vector<CharacterItem> granted_items;
};

// Uses the existing immutable item catalog and inventory representation. Throws on an
// unsupported attachment, a currency/stack limit or insufficient inventory slots.
MailClaimPlan plan_mail_claim(const MailRecord &mail, const Character &character,
                              const WorldProgress &progress, const ShopCatalog &catalog,
                              std::int64_t now_epoch_ticks);

// Validates catalog-backed attachments before durable delivery. Does not write anything.
void validate_mail_catalog(const MailRecord &mail, const ShopCatalog &catalog);

// Protobuf JSON shape of MailContent, suitable for Protocol::encode(174, ...).
nlohmann::json mail_content_json(const MailRecord &mail);

// Emit only after MailRepository::claim_mail returned claimed. The system reason names this mail so
// the native mailbox manager can retire its attachment action when the world reward arrives.
std::vector<GameplayPacket> mail_claim_packets(const MailRecord &mail, const MailClaimPlan &plan);

// Parses the operator API's explicit target and attachments. Unknown keys and malformed
// numeric fields are refused; caller supplies time and authenticates operator privilege.
MailRecord parse_mail_delivery(const nlohmann::json &request, std::int64_t now_epoch_ticks);

} // namespace chronicles
