#include "world/features/mail/mail_rest.hpp"

#include "world/features/mail/mail.hpp"
#include "p_c2f.rest.pb.h"
#include "p_f2c.rest.pb.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <limits>
#include <optional>

namespace chronicles {
namespace {

constexpr std::string_view kFrontPrefix = "/api/front/";
constexpr std::string_view kBearerPrefix = "Bearer ";
constexpr std::string_view kProtobufType = "application/protobuf";
constexpr std::size_t kMaximumRequestBytes = 4096;
constexpr std::size_t kMaximumTokenBytes = 512;

struct Route {
  std::string_view name;
  std::optional<std::int64_t> mail_id;
};

std::optional<Route> parse_route(std::string_view path) {
  path = path.substr(0, path.find('?'));
  const auto prefix = path.find(kFrontPrefix);
  if (prefix == std::string_view::npos) {
    return std::nullopt;
  }
  const auto suffix = path.substr(prefix + kFrontPrefix.size());
  const auto separator = suffix.find('/');
  const auto name = suffix.substr(0, separator);
  if (name == "MailboxGetReq" || name == "MailboxConfirmReq" || name == "MailboxDeleteReq") {
    if (separator != std::string_view::npos) {
      return std::nullopt;
    }
    return Route{name, std::nullopt};
  }
  if (name != "MailboxTakeAttachmentReq" || separator == std::string_view::npos) {
    return std::nullopt;
  }
  const auto text = suffix.substr(separator + 1);
  std::int64_t id{};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), id);
  if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      id <= 0) {
    return std::nullopt;
  }
  return Route{name, id};
}

NativeHttpResponse error(unsigned status, std::string_view text) {
  return {status, "application/json", nlohmann::json{{"error", text}}.dump()};
}

NativeHttpResponse claim_response(MailClaimStatus status) {
  switch (status) {
  case MailClaimStatus::claimed:
  case MailClaimStatus::already_claimed:
    return {204, std::string(kProtobufType), {}};
  case MailClaimStatus::not_found:
    return error(404, "mail_not_found");
  case MailClaimStatus::expired:
    return error(410, "mail_expired");
  case MailClaimStatus::conflict:
    return error(409, "mail_claim_conflict");
  }
  return error(500, "unknown_mail_claim_status");
}

template <typename Message> bool parse_message(const std::string &body, Message &message) {
  return body.size() <= kMaximumRequestBytes &&
         message.ParseFromArray(body.data(), static_cast<int>(body.size()));
}

} // namespace

MailRest::MailRest(AuthService &auth, MailRepository &mail, Claim claim, Now now)
    : auth_(auth), mail_(mail), claim_(std::move(claim)), now_(std::move(now)) {
  if (!claim_ || !now_) {
    throw std::invalid_argument("Mail REST requires a world-session claim callback and clock");
  }
}

bool MailRest::handles(std::string_view path) {
  return parse_route(path).has_value();
}

void MailRest::handle_async(std::string_view method, std::string_view path,
                            std::string_view authorization, std::string_view content_type,
                            const std::string &body, Reply reply) {
  auto completed = std::make_shared<std::atomic_bool>(false);
  Reply complete = [completed, reply = std::move(reply)](NativeHttpResponse response) {
    if (!completed->exchange(true)) {
      reply(std::move(response));
    }
  };
  const auto route = parse_route(path);
  if (!route || !route->mail_id) {
    complete(handle(method, path, authorization, content_type, body));
    return;
  }
  if (method != "POST") {
    complete(error(405, "method_not_allowed"));
    return;
  }
  if (!authorization.starts_with(kBearerPrefix) || authorization.size() <= kBearerPrefix.size() ||
      authorization.size() > kBearerPrefix.size() + kMaximumTokenBytes) {
    complete(error(401, "authentication_required"));
    return;
  }
  if (!body.empty()) {
    complete(error(400, "claim_has_no_body"));
    return;
  }
  try {
    const auto character =
        auth_.selected_character(std::string(authorization.substr(kBearerPrefix.size())));
    if (!character) {
      complete(error(403, "character_not_selected"));
      return;
    }
    claim_(*character, *route->mail_id,
           [complete](MailClaimStatus status) { complete(claim_response(status)); });
  } catch (const Unauthorized &) {
    complete(error(401, "authentication_required"));
  } catch (const std::invalid_argument &) {
    complete(error(400, "invalid_mail_request"));
  } catch (const std::exception &) {
    complete(error(500, "mail_service_unavailable"));
  }
}

NativeHttpResponse MailRest::handle(std::string_view method, std::string_view path,
                                    std::string_view authorization, std::string_view content_type,
                                    const std::string &body) {
  const auto route = parse_route(path);
  if (!route) {
    return error(404, "route_not_found");
  }
  if (!authorization.starts_with(kBearerPrefix) || authorization.size() <= kBearerPrefix.size() ||
      authorization.size() > kBearerPrefix.size() + kMaximumTokenBytes) {
    return error(401, "authentication_required");
  }
  if (body.size() > kMaximumRequestBytes) {
    return error(413, "body_too_large");
  }
  if (method != (route->mail_id ? "POST" : "PUT")) {
    return error(405, "method_not_allowed");
  }
  if (!route->mail_id && !content_type.starts_with(kProtobufType)) {
    return error(415, "protobuf_required");
  }
  try {
    const auto character =
        auth_.selected_character(std::string(authorization.substr(kBearerPrefix.size())));
    if (!character) {
      return error(403, "character_not_selected");
    }
    if (route->mail_id) {
      return error(409, "mail_claim_requires_async_dispatch");
    }
    const auto now = now_();
    if (route->name == "MailboxGetReq") {
      x5::pb::rest::MailboxGetReq request;
      if (!parse_message(body, request) || request.count() > kMaximumMailBatch ||
          request.start() > kMaximumMailboxEntries) {
        return error(400, "invalid_mail_page");
      }
      const auto mails =
          mail_.mailbox(character->account_id, character->id, character->world_id, now);
      x5::pb::rest::MailboxGetRes response;
      response.set_start(request.start());
      response.set_total_count(static_cast<std::uint32_t>(mails.size()));
      const auto begin = std::min<std::size_t>(request.start(), mails.size());
      const auto end = std::min<std::size_t>(begin + request.count(), mails.size());
      for (auto index = begin; index < end; ++index) {
        parse_json(mail_content_json(mails[index]), *response.add_contents());
      }
      NativeHttpResponse result;
      if (!response.SerializeToString(&result.body)) {
        return error(500, "mail_serialization_failed");
      }
      return result;
    }
    std::vector<std::int64_t> ids;
    const bool remove = route->name == "MailboxDeleteReq";
    if (remove) {
      x5::pb::rest::MailboxDeleteReq request;
      if (!parse_message(body, request)) {
        return error(400, "invalid_mail_batch");
      }
      ids.assign(request.mail_ids().begin(), request.mail_ids().end());
    } else {
      x5::pb::rest::MailboxConfirmReq request;
      if (!parse_message(body, request)) {
        return error(400, "invalid_mail_batch");
      }
      ids.assign(request.mail_ids().begin(), request.mail_ids().end());
    }
    if (!mail_.change_mail(character->account_id, character->id, character->world_id, ids, remove,
                           now)) {
      return error(409, "mail_batch_refused");
    }
    return {204, std::string(kProtobufType), {}};
  } catch (const Unauthorized &) {
    return error(401, "authentication_required");
  } catch (const std::invalid_argument &) {
    return error(400, "invalid_mail_request");
  } catch (const std::exception &) {
    return error(500, "mail_service_unavailable");
  }
}

} // namespace chronicles
