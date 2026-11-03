#include "world/features/mail/mail_types.hpp"

#include <nlohmann/json.hpp>
#include <stdexcept>

namespace chronicles {
namespace {

void validate_text(const std::string &text, std::size_t maximum, bool allow_newlines) {
  if (text.size() > maximum) {
    throw std::invalid_argument("Mail text exceeds its byte limit");
  }
  for (const auto character : text) {
    const auto byte = static_cast<unsigned char>(character);
    if ((byte < 32 && !(allow_newlines && (byte == '\n' || byte == '\t'))) || byte == 127) {
      throw std::invalid_argument("Mail text contains an unsupported control character");
    }
  }
  // nlohmann validates UTF-8 when serializing; reject bad input before any storage write.
  static_cast<void>(nlohmann::json(text).dump());
}

} // namespace

void validate_mail_delivery(const MailRecord &mail) {
  if (mail.account_id <= 0 || mail.character_id < 0 || mail.world_id <= 0 || mail.type < 0 ||
      mail.type > 1 || mail.title.empty() || mail.delivery_key.empty() ||
      mail.sent_epoch_ticks <= 0 || mail.expiry_epoch_ticks <= mail.sent_epoch_ticks ||
      mail.attachment_expiry_epoch_ticks <= mail.sent_epoch_ticks ||
      mail.attachment_expiry_epoch_ticks > mail.expiry_epoch_ticks ||
      mail.attachments.size() > kMaximumMailAttachments || mail.confirmed_epoch_ticks != 0 ||
      mail.claimed_epoch_ticks != 0 || mail.claimed_character_id != 0 || mail.deleted) {
    throw std::invalid_argument("Invalid mail delivery target, times or state");
  }
  validate_text(mail.title, 256, false);
  validate_text(mail.text, 16'384, true);
  validate_text(mail.delivery_key, 128, false);
  for (const auto &attachment : mail.attachments) {
    if (attachment.kind != MailAttachmentKind::item &&
        attachment.kind != MailAttachmentKind::money) {
      throw std::invalid_argument("Invalid mail attachment kind");
    }
    if ((attachment.kind == MailAttachmentKind::item && attachment.cms_id <= 0) ||
        attachment.count <= 0 ||
        (attachment.grade && (*attachment.grade < 0 || *attachment.grade > 5))) {
      throw std::invalid_argument("Invalid mail attachment");
    }
    if (attachment.kind == MailAttachmentKind::money &&
        ((attachment.cms_id != 0 && attachment.cms_id != 1) || attachment.grade)) {
      throw std::invalid_argument("Mail supports Gold and HersanderToken currencies");
    }
  }
}

} // namespace chronicles
