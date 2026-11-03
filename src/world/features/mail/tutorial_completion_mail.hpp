#pragma once

#include "world/features/mail/mail_types.hpp"

#include <cstdint>
#include <optional>

namespace chronicles {

class MailRepository;
class ShopCatalog;

// Emulator policy, not recovered official behavior: one welcome mail of free supplies for each
// account and realm, delivered when one of its characters finishes the tutorial. The delivery
// key makes the grant idempotent across characters, reconnects and retries. Change the key
// version only when intentionally granting a new, separate reward to every account again;
// reusing a key with different content is refused by the store.
inline constexpr char kTutorialCompletionMailKey[] = "tutorial-completion-supplies-v1";

// Builds the validated, account-addressed welcome mail. Throws std::invalid_argument when the
// reward definition is invalid or exceeds mail limits.
MailRecord build_tutorial_completion_mail(std::int64_t account_id, int world_id,
                                          std::int64_t now_epoch_ticks);

// Delivers the welcome mail through the store and returns it only when this call created it,
// so the caller notifies the online player once. Returns nullopt when the account already
// received the reward, or when the recipient or mailbox is unavailable. Throws
// std::invalid_argument for an invalid reward (for example an item missing from `catalog`) and
// propagates storage failures.
std::optional<MailRecord> deliver_tutorial_completion_mail(MailRepository &mail,
                                                           const ShopCatalog &catalog,
                                                           std::int64_t account_id, int world_id,
                                                           std::int64_t now_epoch_ticks);

} // namespace chronicles
