#include "services/front/native_ws.hpp"

#include "world/features/friends/friends.hpp"
#include "world/features/guild/guild.hpp"
#include "world/features/friends/native_friend.hpp"
#include "world/features/guild/native_guild.hpp"
#include "world/features/housing/native_housing.hpp"
#include "world/features/chat/player_commands.hpp"
#include "p_system.pb.h"

#include <spdlog/spdlog.h>
#include <stdexcept>

namespace chronicles {
namespace {

using native_ws::parse_payload;

std::string chat_error(x5::ec::EErrorCode code) {
  // WSF2C_CommonErrorRes ID 1 is verified in native-ws-opcodes.json.
  x5::pb::CommonErrorRes response;
  response.set_ec(code);
  return native_ws::encode_frame(1, response);
}

} // namespace

NativeWsDispatcher::NativeWsDispatcher(AuthService &auth, std::shared_ptr<GuildService> guilds,
                                       std::shared_ptr<housing::HousingHub> housing,
                                       std::shared_ptr<FriendService> friends,
                                       std::shared_ptr<const CompanyService> companies,
                                       std::shared_ptr<GuildHousingRequests> guild_housing)
    : auth_(auth), chat_(auth), commands_(std::make_shared<PlayerCommands>()),
      guilds_(std::move(guilds)), friends_(std::move(friends)) {
  if (housing) {
    housing_front_ = std::make_shared<NativeHousingFront>(auth_, housing, chat_, companies);
    std::weak_ptr<NativeHousingFront> weak_housing = housing_front_;
    housing->set_guild_area_listener([weak_housing](const housing::GuildAreaNotice &notice) {
      const auto front = weak_housing.lock();
      if (front) {
        front->deliver_guild_area(notice);
      }
    });
  }

  if (friends_) {
    friend_front_ = std::make_shared<NativeFriendFront>(auth_, *friends_, chat_);
    std::weak_ptr<NativeFriendFront> weak_friend_front = friend_front_;
    friends_->subscribe([weak_friend_front](const std::vector<FriendEvent> &events) {
      const auto front = weak_friend_front.lock();
      if (front) {
        front->deliver(events);
      }
    });
  }

  if (!guilds_) {
    return;
  }

  guild_front_ = std::make_shared<NativeGuildFront>(auth_, *guilds_, chat_, std::move(companies),
                                                    guild_housing);
  std::weak_ptr<NativeGuildFront> weak_front = guild_front_;
  guilds_->subscribe([weak_front](const std::vector<GuildEvent> &events) {
    const auto front = weak_front.lock();
    if (front) {
      front->deliver(events);
    }
  });

  // Guild housing answers contracts asynchronously and notifies members through the same
  // per-character front queues.
  if (guild_housing && !housing) {
    guild_housing->set_front_delivery(
        [weak_front](std::int64_t char_id, const std::string &frame) {
          const auto front = weak_front.lock();
          if (front) {
            front->push_frame(char_id, frame);
          }
        });
  }
}

NativeWsDispatcher::NativeWsDispatcher(AuthService &auth, NativeWsServices services)
    : NativeWsDispatcher(auth, std::move(services.guilds), {}, {},
                         std::move(services.companies), std::move(services.guild_housing)) {}

NativeChatService &NativeWsDispatcher::chat() {
  return chat_;
}

void NativeWsDispatcher::set_player_commands(std::shared_ptr<PlayerCommands> commands) {
  if (!commands) {
    throw std::invalid_argument("Player command dispatcher cannot be null");
  }
  commands_ = std::move(commands);
}

void NativeWsDispatcher::set_local_chat_audience(NativeChatService::ChatAudience audience) {
  if (!audience) {
    throw std::invalid_argument("Local chat audience resolver cannot be empty");
  }
  local_chat_audience_ = std::move(audience);
}

std::string NativeWsDispatcher::handle(NativeWsChannel channel, const std::string &token,
                                       std::string_view message) {
  if (!auth_.authenticated(token)) {
    throw Unauthorized();
  }

  const auto frames = native_ws::decode_frames(message);
  const auto *channel_name = channel == NativeWsChannel::auth ? "auth" : "front";
  spdlog::info("Native WebSocket service={} packets={} bytes={}", channel_name, frames.size(),
               message.size());

  std::string response;
  for (const auto frame : frames) {
    spdlog::info("Native WebSocket service={} packet_id={} payload_bytes={}", channel_name,
                 frame.packet_id, frame.payload.size());
    std::string frame_response;
    switch (channel) {
    case NativeWsChannel::auth:
      frame_response = handle_auth(token, frame);
      break;
    case NativeWsChannel::front:
      frame_response = handle_front(token, frame);
      break;
    default:
      throw std::invalid_argument("Unknown native WebSocket channel");
    }

    if (frame_response.size() > native_ws::kMaximumMessageBytes - response.size()) {
      throw std::length_error("Native WebSocket responses exceed the message size limit");
    }
    response.append(frame_response);
  }

  return response;
}

std::string NativeWsDispatcher::handle_auth(const std::string &token, native_ws::FrameView frame) {
  if (frame.packet_id != native_ws::kWorldEnterRequest) {
    throw std::invalid_argument("Unsupported native auth WebSocket packet ID " +
                                std::to_string(frame.packet_id));
  }

  const auto request = parse_payload<x5::pb::authwebsocket::WorldEnterReq>(frame.payload);
  return native_ws::encode_frame(native_ws::kWorldEnterResponse, auth_.enter(token, request));
}

std::string NativeWsDispatcher::handle_front(const std::string &token, native_ws::FrameView frame) {
  if (housing_front_ && NativeHousingFront::handles(frame.packet_id)) {
    return housing_front_->handle(token, frame);
  }
  if (guild_front_ && guild_front_->handles(frame.packet_id)) {
    return guild_front_->handle(token, frame);
  }
  if (friend_front_ && NativeFriendFront::handles(frame.packet_id)) {
    return friend_front_->handle(token, frame);
  }

  switch (frame.packet_id) {
  case native_ws::kChatRequest:
    return handle_chat(token, frame);
  case native_ws::kDirectMessageRequest: {
    const auto request = parse_payload<x5::pb::ws::front::DirectMessageReq>(frame.payload);
    try {
      return native_ws::encode_frame(native_ws::kDirectMessageResponse,
                                     chat_.direct_message(token, request));
    } catch (const std::invalid_argument &) {
      return chat_error(x5::ec::BadRequest);
    } catch (const std::length_error &) {
      return chat_error(x5::ec::ServerOverload);
    }
  }
  case native_ws::kCharacterListRequest: {
    parse_payload<x5::pb::ws::front::CharListReq>(frame.payload);
    return native_ws::encode_frame(native_ws::kCharacterListResponse, auth_.list(token));
  }
  case native_ws::kCharacterCreateRequest: {
    const auto request = parse_payload<x5::pb::ws::front::CharCreateReq>(frame.payload);
    return native_ws::encode_frame(native_ws::kCharacterCreateResponse,
                                   auth_.create(token, request));
  }
  case native_ws::kCharacterSelectRequest: {
    const auto request = parse_payload<x5::pb::ws::front::CharSelectReq>(frame.payload);
    auto response = auth_.select(token, request);
    if (housing_front_ && response.ec() == x5::ec::Success) {
      housing_front_->populate_character_entry(token, *response.mutable_character());
    }
    return native_ws::encode_frame(native_ws::kCharacterSelectResponse, response);
  }
  case native_ws::kCharacterDeletePreCheckRequest: {
    const auto request = parse_payload<x5::pb::ws::front::CharDeletePreCheckReq>(frame.payload);
    return native_ws::encode_frame(native_ws::kCharacterDeletePreCheckResponse,
                                   auth_.delete_precheck(token, request));
  }
  case native_ws::kCharacterDeleteRequest: {
    const auto request = parse_payload<x5::pb::ws::front::CharDeleteReq>(frame.payload);
    return native_ws::encode_frame(native_ws::kCharacterDeleteResponse,
                                   auth_.delete_character(token, request));
  }
  case native_ws::kCharacterDeleteCancelRequest: {
    const auto request = parse_payload<x5::pb::ws::front::CharDeleteCancelReq>(frame.payload);
    return native_ws::encode_frame(native_ws::kCharacterDeleteCancelResponse,
                                   auth_.delete_cancel(token, request));
  }
  case native_ws::kCharacterDeleteImmediateRequest: {
    const auto request = parse_payload<x5::pb::ws::front::CharDeleteImmediateReq>(frame.payload);
    return native_ws::encode_frame(native_ws::kCharacterDeleteImmediateResponse,
                                   auth_.delete_immediate(token, request));
  }
  case native_ws::kLookupCharIdByNameRequest: {
    const auto request = parse_payload<x5::pb::ws::front::LookupCharIdByNameReq>(frame.payload);
    return native_ws::encode_frame(native_ws::kLookupCharIdByNameResponse,
                                   auth_.lookup_char_id(token, request));
  }
  case native_ws::kFriendListRequest: {
    const auto request = parse_payload<x5::pb::ws::front::FriendListReq>(frame.payload);
    return native_ws::encode_frame(native_ws::kFriendListResponse, auth_.friends(token, request));
  }
  default:
    throw std::invalid_argument("Unsupported native front WebSocket packet ID " +
                                std::to_string(frame.packet_id));
  }
}

std::string NativeWsDispatcher::handle_chat(const std::string &token, native_ws::FrameView frame) {
  const auto request = parse_payload<x5::pb::ws::front::ChatReq>(frame.payload);
  try {
    if (!request.message().empty() &&
        (request.message().front() == '/' || request.message().front() == '!')) {
      const auto identity = auth_.chat_identity(token);
      if (!identity) {
        throw Unauthorized();
      }
      const auto result = commands_->execute(*identity, request.message());
      for (const auto &reply : result.replies) {
        chat_.private_notice(identity->world_id, identity->character_id, "[Server]", reply);
      }
      return {};
    }

    // The Guild channel's audience is the sender's guild, resolved from the guild registry
    // for the authenticated character. Other channels keep their own rules in world_chat.
    if (guilds_ && request.type() == x5::cms::EX5ChatMessageType_Guild) {
      const auto audience = [this](const ChatIdentity &sender) {
        return guilds_->guild_mates(sender.character_id);
      };
      if (!chat_.group_chat(token, request, audience)) {
        spdlog::info("Front chat type={} refused: sender has no guild",
                     static_cast<int>(request.type()));
        return chat_error(x5::ec::Guild_NotJoined);
      }
      return {};
    }

    // Local is the client's default channel. Its audience comes from the world service's
    // sight registry; an empty audience means the speaker is not currently in the world.
    if (local_chat_audience_ && request.type() == x5::cms::EX5ChatMessageType_Local) {
      if (!chat_.group_chat(token, request, local_chat_audience_)) {
        spdlog::info("Front chat type={} refused: speaker is not in the world",
                     static_cast<int>(request.type()));
        return chat_error(x5::ec::BadRequest);
      }
      return {};
    }

    chat_.world_chat(token, request);
  } catch (const std::invalid_argument &error) {
    // Chat text is private; only the channel and the refusal reason are logged.
    spdlog::info("Front chat type={} refused: {}", static_cast<int>(request.type()), error.what());
    return chat_error(x5::ec::BadRequest);
  } catch (const std::length_error &error) {
    spdlog::info("Front chat type={} refused: {}", static_cast<int>(request.type()), error.what());
    return chat_error(x5::ec::ServerOverload);
  }
  return {};
}

} // namespace chronicles
