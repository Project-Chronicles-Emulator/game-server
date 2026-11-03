#include "net/world_tls_diagnostics.hpp"

#include <spdlog/spdlog.h>

#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace chronicles {
namespace {

int tls_diagnostics_index() noexcept {
  static const int index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
  return index;
}

void record_tls_info(const SSL *connection, int event, int result) noexcept {
  auto *diagnostics =
      static_cast<WorldTlsDiagnostics *>(SSL_get_ex_data(connection, tls_diagnostics_index()));
  if (!diagnostics) {
    return;
  }

  // OpenSSL supplies static state descriptions. Record only metadata here, keeping
  // allocation and logging outside the C callback and away from raw TLS records.
  if ((event & SSL_CB_LOOP) != 0) {
    diagnostics->last_state = SSL_state_string_long(connection);
  }
  if ((event & SSL_CB_ALERT) != 0) {
    diagnostics->alert_seen = true;
    diagnostics->alert_received = (event & SSL_CB_READ) != 0;
    diagnostics->alert_value = result;
    diagnostics->alert_state = SSL_state_string_long(connection);
  }
}

void record_tls_message(int sent, int, int content_type, const void *buffer, std::size_t bytes,
                        SSL *, void *argument) noexcept {
  auto *diagnostics = static_cast<WorldTlsDiagnostics *>(argument);
  if (!diagnostics || (content_type != SSL3_RT_HANDSHAKE && content_type != SSL3_RT_ALERT &&
                       content_type != SSL3_RT_CHANGE_CIPHER_SPEC)) {
    return;
  }
  if (diagnostics->message_count == diagnostics->messages.size()) {
    if (diagnostics->dropped_messages != (std::numeric_limits<std::size_t>::max)()) {
      ++diagnostics->dropped_messages;
    }
    return;
  }

  // The callback receives plaintext protocol messages, including certificates and
  // key exchange material. Inspect only the one-byte handshake type; retain no payload.
  unsigned message_type{};
  if (content_type == SSL3_RT_HANDSHAKE && buffer && bytes > 0) {
    message_type = *static_cast<const unsigned char *>(buffer);
  }
  diagnostics->messages[diagnostics->message_count] =
      WorldTlsMessageMetadata{sent != 0, content_type, message_type, bytes};
  ++diagnostics->message_count;
}

const char *tls_handshake_message_name(unsigned type) noexcept {
  switch (type) {
  case SSL3_MT_CLIENT_HELLO:
    return "client_hello";
  case SSL3_MT_SERVER_HELLO:
    return "server_hello";
  case SSL3_MT_NEWSESSION_TICKET:
    return "new_session_ticket";
  case SSL3_MT_END_OF_EARLY_DATA:
    return "end_of_early_data";
  case SSL3_MT_ENCRYPTED_EXTENSIONS:
    return "encrypted_extensions";
  case SSL3_MT_CERTIFICATE:
    return "certificate";
  case SSL3_MT_SERVER_KEY_EXCHANGE:
    return "server_key_exchange";
  case SSL3_MT_CERTIFICATE_REQUEST:
    return "certificate_request";
  case SSL3_MT_SERVER_DONE:
    return "server_hello_done";
  case SSL3_MT_CERTIFICATE_VERIFY:
    return "certificate_verify";
  case SSL3_MT_CLIENT_KEY_EXCHANGE:
    return "client_key_exchange";
  case SSL3_MT_FINISHED:
    return "finished";
  case SSL3_MT_KEY_UPDATE:
    return "key_update";
  default:
    return "handshake";
  }
}

std::string tls_message_trace(const WorldTlsDiagnostics &diagnostics) {
  std::string trace;
  for (std::size_t index = 0; index < diagnostics.message_count; ++index) {
    const auto &message = diagnostics.messages[index];
    auto name = "change_cipher_spec";
    if (message.content_type == SSL3_RT_HANDSHAKE) {
      name = tls_handshake_message_name(message.message_type);
    } else if (message.content_type == SSL3_RT_ALERT) {
      name = "alert";
    }

    if (!trace.empty()) {
      trace += ',';
    }
    trace += fmt::format("{}:{}(", message.sent ? "sent" : "received", name);
    if (message.content_type == SSL3_RT_HANDSHAKE) {
      trace += fmt::format("type={},", message.message_type);
    }
    trace += fmt::format("bytes={})", message.bytes);
  }
  return trace.empty() ? "none" : trace;
}

std::string_view diagnostic_server_name(const SSL *connection) {
  const auto *name = SSL_get_servername(connection, TLSEXT_NAMETYPE_host_name);
  if (!name) {
    return "none";
  }

  const std::string_view value(name);
  constexpr std::string_view kSafeServerNameCharacters =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-:_[]";
  if (value.empty() || value.size() > 253 ||
      value.find_first_not_of(kSafeServerNameCharacters) != std::string_view::npos) {
    return "invalid-or-unprintable";
  }
  return value;
}

} // namespace

void attach_world_tls_diagnostics(SSL *connection, WorldTlsDiagnostics &diagnostics) {
  const auto index = tls_diagnostics_index();
  if (!connection || index < 0 || SSL_set_ex_data(connection, index, &diagnostics) != 1) {
    throw std::runtime_error("Cannot initialize world TLS diagnostics");
  }
  SSL_set_info_callback(connection, record_tls_info);
  SSL_set_msg_callback(connection, record_tls_message);
  SSL_set_msg_callback_arg(connection, &diagnostics);
}

void log_world_tls_result(const SSL *connection, const WorldTlsDiagnostics &diagnostics,
                          boost::system::error_code error) {
  const auto *cipher = SSL_get_current_cipher(connection);
  auto alert_direction = "none";
  auto alert_description = "none";
  auto alert_level = "none";
  if (diagnostics.alert_seen) {
    alert_direction = diagnostics.alert_received ? "received" : "sent";
    alert_description = SSL_alert_desc_string_long(diagnostics.alert_value);
    alert_level = SSL_alert_type_string_long(diagnostics.alert_value);
  }

  spdlog::log(
      error ? spdlog::level::warn : spdlog::level::info,
      "World TLS connection={} outcome={} version={} cipher={} sni={} server_state={} "
      "last_server_state={} alert_direction={} alert_level={} alert={} alert_server_state={} "
      "error_category={} error_code={}",
      diagnostics.connection_id, error ? "failed" : "completed", SSL_get_version(connection),
      cipher ? SSL_CIPHER_get_name(cipher) : "none", diagnostic_server_name(connection),
      SSL_state_string_long(connection), diagnostics.last_state, alert_direction, alert_level,
      alert_description, diagnostics.alert_state, error.category().name(), error.value());
  spdlog::log(error ? spdlog::level::warn : spdlog::level::info,
              "World TLS message trace connection={} observed={} dropped_messages={}",
              diagnostics.connection_id, tls_message_trace(diagnostics),
              diagnostics.dropped_messages);
}

} // namespace chronicles
