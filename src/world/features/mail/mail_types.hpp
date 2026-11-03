#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace chronicles {

inline constexpr std::size_t kMaximumMailAttachments = 32;
inline constexpr std::size_t kMaximumMailboxEntries = 1000;
inline constexpr std::size_t kMaximumMailBatch = 100;

enum class MailAttachmentKind { item, money };

struct MailAttachment {
  MailAttachmentKind kind{MailAttachmentKind::item};
  // Item CMS id, or EX5MoneyType (0 Gold, 1 HersanderToken).
  int cms_id{};
  int count{};
  std::optional<int> grade;

  bool operator==(const MailAttachment &) const = default;
};

struct MailRecord {
  std::int64_t id{};
  std::int64_t account_id{};
  // Zero addresses account mail, claimed once by any character in this world.
  std::int64_t character_id{};
  int world_id{1};
  // The operator's stable delivery key makes an uncertain delivery retry idempotent.
  std::string delivery_key;
  int type{1};
  std::string title;
  std::string text;
  std::int64_t sent_epoch_ticks{};
  std::int64_t expiry_epoch_ticks{};
  std::int64_t attachment_expiry_epoch_ticks{};
  std::int64_t confirmed_epoch_ticks{};
  std::int64_t claimed_epoch_ticks{};
  std::int64_t claimed_character_id{};
  bool deleted{};
  std::vector<MailAttachment> attachments;

  bool operator==(const MailRecord &) const = default;
};

enum class MailClaimStatus { claimed, already_claimed, not_found, expired, conflict };

// Throws for malformed operator input; this does not establish catalog validity.
void validate_mail_delivery(const MailRecord &mail);

} // namespace chronicles
