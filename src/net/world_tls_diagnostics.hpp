#pragma once

#include <boost/system/error_code.hpp>
#include <openssl/ssl.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace chronicles {

struct WorldTlsMessageMetadata {
  bool sent{};
  int content_type{};
  unsigned message_type{};
  std::size_t bytes{};
};

// The stream's OpenSSL callbacks point to this object. Declare it before the SSL stream
// so destruction frees the stream before this metadata. Never retain plaintext payloads.
struct WorldTlsDiagnostics {
  static constexpr std::size_t kMaximumMessages = 64;

  std::uint64_t connection_id{};
  const char *last_state{"not started"};
  const char *alert_state{"none"};
  int alert_value{};
  bool alert_seen{};
  bool alert_received{};
  std::array<WorldTlsMessageMetadata, kMaximumMessages> messages{};
  std::size_t message_count{};
  std::size_t dropped_messages{};
};

void attach_world_tls_diagnostics(SSL *connection, WorldTlsDiagnostics &diagnostics);

void log_world_tls_result(const SSL *connection, const WorldTlsDiagnostics &diagnostics,
                          boost::system::error_code error);

} // namespace chronicles
