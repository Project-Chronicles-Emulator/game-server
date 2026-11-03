#include "world/features/mail/tutorial_completion_mail.hpp"

#include "world/features/mail/mail.hpp"
#include "world/features/mail/mail_repository.hpp"
#include "data/shop_catalog.hpp"

#include <array>
#include <stdexcept>

namespace chronicles {
namespace {

constexpr std::int64_t kEpochTicksPerSecond = 10'000'000;
constexpr std::int64_t kRetentionSeconds = 90LL * 24 * 60 * 60;

// Recovered ordinary consumables that the tutorial itself hands out (docs/item-use.md).
constexpr int kHealthPotionItemId = 40101101;
constexpr int kStaminaPotionItemId = 40301201;
constexpr int kHealthPotionCount = 20;
constexpr int kStaminaPotionCount = 10;

constexpr char kMailTitle[] = "Welcome, adventurer!";
constexpr char kMailText[] =
    "Congratulations on completing the tutorial! Here are some free supplies to help you on "
    "your journey. Claim the attachments from this mail.";

constexpr std::array<MailAttachment, 2> kRewardAttachments{{
    {MailAttachmentKind::item, kHealthPotionItemId, kHealthPotionCount, {}},
    {MailAttachmentKind::item, kStaminaPotionItemId, kStaminaPotionCount, {}},
}};

} // namespace

MailRecord build_tutorial_completion_mail(std::int64_t account_id, int world_id,
                                          std::int64_t now_epoch_ticks) {
  MailRecord mail;
  mail.account_id = account_id;
  mail.character_id = 0;
  mail.world_id = world_id;
  mail.delivery_key = kTutorialCompletionMailKey;
  mail.type = 1;
  mail.title = kMailTitle;
  mail.text = kMailText;
  mail.sent_epoch_ticks = now_epoch_ticks;
  mail.expiry_epoch_ticks = now_epoch_ticks + kRetentionSeconds * kEpochTicksPerSecond;
  mail.attachment_expiry_epoch_ticks = mail.expiry_epoch_ticks;
  mail.attachments.assign(kRewardAttachments.begin(), kRewardAttachments.end());

  validate_mail_delivery(mail);
  return mail;
}

std::optional<MailRecord> deliver_tutorial_completion_mail(MailRepository &mail,
                                                           const ShopCatalog &catalog,
                                                           std::int64_t account_id, int world_id,
                                                           std::int64_t now_epoch_ticks) {
  if (now_epoch_ticks <= 0) {
    throw std::invalid_argument("Tutorial completion mail requires a positive timestamp");
  }

  const auto draft = build_tutorial_completion_mail(account_id, world_id, now_epoch_ticks);
  validate_mail_catalog(draft, catalog);

  const auto delivered = mail.deliver_mail(draft);
  if (!delivered) {
    return std::nullopt;
  }

  // A repeated delivery key returns the stored original, whose send time predates this call.
  // That identifies a mail this call did not create, so the player is not notified again.
  if (delivered->sent_epoch_ticks != draft.sent_epoch_ticks) {
    return std::nullopt;
  }

  return delivered;
}

} // namespace chronicles
