#pragma once

#include "protocol/gameplay_packet.hpp"

#include <string>

namespace chronicles {

// Display style of an operator notice (x5.cms.EX5SystemMessageType values 1-3).
enum class SystemNoticeStyle {
  text = 1,
  banner = 2,
  popup = 3,
};

namespace chat {

// Existing operator notice wire body; this does not choose the addressed world sessions.
GameplayPacket system_notice_packet(const std::string &text, SystemNoticeStyle style);

} // namespace chat
} // namespace chronicles
