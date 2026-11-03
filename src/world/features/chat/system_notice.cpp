#include "world/features/chat/system_notice.hpp"

namespace chronicles::chat {
namespace {

// PS2C_SystemMessageNoti. Official captures only set the CMS `key`; the operator console's
// free-text notice uses `text` instead, which the original client has not been observed to show.
constexpr std::uint16_t kSystemMessageNotification = 3;

// x5.cms.EX5SystemMessageType enum names for the protobuf JSON codec.
const char *system_notice_type_name(SystemNoticeStyle style) {
  switch (style) {
  case SystemNoticeStyle::banner:
    return "EX5SystemMessageType_SystemMessage_Banner";
  case SystemNoticeStyle::popup:
    return "EX5SystemMessageType_SystemMessage_Popup";
  case SystemNoticeStyle::text:
    break;
  }
  return "EX5SystemMessageType_SystemMessage_Text";
}

} // namespace

GameplayPacket system_notice_packet(const std::string &text, SystemNoticeStyle style) {
  return {kSystemMessageNotification, {{"text", text}, {"type", system_notice_type_name(style)}}};
}

} // namespace chronicles::chat
