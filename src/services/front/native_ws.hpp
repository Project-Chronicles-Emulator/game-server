#pragma once

#include "auth/auth.hpp"
#include "net/native_ws_frames.hpp"
#include "world/features/chat/native_chat.hpp"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace chronicles {

class FriendService;
class CompanyService;
class GuildHousingRequests;
class GuildService;
class NativeFriendFront;
class NativeGuildFront;
class NativeHousingFront;
class PlayerCommands;

namespace housing {
class HousingHub;
} // namespace housing

enum class NativeWsChannel { auth, front };

// Realm services the front WebSocket routes to. Each one is optional.
struct NativeWsServices {
  // Enables the WSC2F guild requests and the Guild chat channel.
  std::shared_ptr<GuildService> guilds;
  // Supplies company membership for guild admission.
  std::shared_ptr<const CompanyService> companies;
  // Enables the guild housing requests 20034-20038. Requires `guilds`.
  std::shared_ptr<GuildHousingRequests> guild_housing;
};

class NativeWsDispatcher {
public:
  // Optional services preserve the same front dispatcher for local and live profiles.
  explicit NativeWsDispatcher(AuthService &auth, std::shared_ptr<GuildService> guilds = {},
                              std::shared_ptr<housing::HousingHub> housing = {},
                              std::shared_ptr<FriendService> friends = {},
                              std::shared_ptr<const CompanyService> companies = {},
                              std::shared_ptr<GuildHousingRequests> guild_housing = {});
  explicit NativeWsDispatcher(AuthService &auth, NativeWsServices services);

  // The transport must send the result as binary and close on malformed input.
  // Authentication is checked on every message, including an already upgraded socket.
  std::string handle(NativeWsChannel channel, const std::string &token, std::string_view message);

  NativeChatService &chat();

  // Configure before accepting connections. Slash replies remain private in every channel.
  void set_player_commands(std::shared_ptr<PlayerCommands> commands);

  // Configure before accepting connections. Resolves the Local channel's audience (the
  // speaker plus the characters that currently see the speaker in the world). Without it,
  // Local chat is refused rather than broadcast to the whole realm.
  void set_local_chat_audience(NativeChatService::ChatAudience audience);

private:
  std::string handle_auth(const std::string &token, native_ws::FrameView frame);

  std::string handle_front(const std::string &token, native_ws::FrameView frame);

  std::string handle_chat(const std::string &token, native_ws::FrameView frame);

  AuthService &auth_;
  NativeChatService chat_;
  std::shared_ptr<PlayerCommands> commands_;
  NativeChatService::ChatAudience local_chat_audience_;
  std::shared_ptr<GuildService> guilds_;
  // Shared so the GuildService subscription can hold a weak reference.
  std::shared_ptr<NativeGuildFront> guild_front_;
  // Shared so the housing hub's guild-area listener can hold a weak reference.
  std::shared_ptr<NativeHousingFront> housing_front_;
  std::shared_ptr<FriendService> friends_;
  // Shared so the FriendService subscription can hold a weak reference.
  std::shared_ptr<NativeFriendFront> friend_front_;
};

} // namespace chronicles
