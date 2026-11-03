#include "world/features/chat/system_notice.hpp"

#include <gtest/gtest.h>

namespace chronicles {
namespace {

using Json = nlohmann::json;

TEST(ChatSystemNotice, KeepsTheOriginalStylesOpcodeAndTextWithoutAddingAnAudienceDecision) {
  const auto text = chat::system_notice_packet("Visible notice", SystemNoticeStyle::text);
  const auto banner = chat::system_notice_packet("Visible notice", SystemNoticeStyle::banner);
  const auto popup = chat::system_notice_packet("Visible notice", SystemNoticeStyle::popup);
  const auto fallback = chat::system_notice_packet("", static_cast<SystemNoticeStyle>(99));
  EXPECT_EQ(text.opcode, 3);
  EXPECT_EQ(text.message, (Json{{"text", "Visible notice"},
                                {"type", "EX5SystemMessageType_SystemMessage_Text"}}));
  EXPECT_EQ(banner.message.at("type"), "EX5SystemMessageType_SystemMessage_Banner");
  EXPECT_EQ(popup.message.at("type"), "EX5SystemMessageType_SystemMessage_Popup");
  EXPECT_EQ(fallback.message,
            (Json{{"text", ""}, {"type", "EX5SystemMessageType_SystemMessage_Text"}}));
}

} // namespace
} // namespace chronicles
