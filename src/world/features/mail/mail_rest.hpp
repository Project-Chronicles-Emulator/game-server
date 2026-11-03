#pragma once

#include "auth/auth.hpp"
#include "auth/native_auth.hpp"
#include "world/features/mail/mail_repository.hpp"

#include <functional>
#include <string_view>

namespace chronicles {

class MailRest {
public:
  // The callback must run the claim on the active world-session worker and update every
  // gameplay cache plus its item/money notifications before reporting success. An absent
  // active world session must report conflict. No worker may wait for another worker.
  using ClaimReply = std::function<void(MailClaimStatus)>;
  using Claim = std::function<void(const Character &, std::int64_t, ClaimReply)>;
  using Reply = std::function<void(NativeHttpResponse)>;
  using Now = std::function<std::int64_t()>;

  MailRest(AuthService &auth, MailRepository &mail, Claim claim, Now now);

  static bool handles(std::string_view path);

  void handle_async(std::string_view method, std::string_view path, std::string_view authorization,
                    std::string_view content_type, const std::string &body, Reply reply);

  // Non-claim operations can be served synchronously on the existing HTTP worker.
  NativeHttpResponse handle(std::string_view method, std::string_view path,
                            std::string_view authorization, std::string_view content_type,
                            const std::string &body);

private:
  AuthService &auth_;
  MailRepository &mail_;
  Claim claim_;
  Now now_;
};

} // namespace chronicles
