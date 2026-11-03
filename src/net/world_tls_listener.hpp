#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>

#include <filesystem>
#include <functional>
#include <memory>

namespace chronicles {

struct WorldTlsOptions {
  std::filesystem::path certificate_file;
  std::filesystem::path private_key_file;
  bool tls12_only{};
};

// The callback owns the accepted socket and must retain the context for any SSL stream it
// creates. World admission, capacity checks and session construction belong to the caller.
using AcceptedWorldSocket =
    std::function<void(boost::asio::ip::tcp::socket, std::shared_ptr<boost::asio::ssl::context>)>;

class WorldTlsListener {
public:
  virtual ~WorldTlsListener() = default;

  // These operations share the listener's single I/O executor. Call stop before destroying
  // its io_context; stop cancels acceptance and does not close sockets already handed off.
  virtual unsigned short port() const = 0;
  virtual bool is_open() const = 0;
  virtual void stop() = 0;
};

// Binds and configures TLS synchronously, then schedules the first accept. Configuration
// failures throw before this function returns. The caller must keep io_context alive.
std::shared_ptr<WorldTlsListener>
start_world_tls_listener(boost::asio::io_context &io_context,
                         const boost::asio::ip::tcp::endpoint &endpoint,
                         const WorldTlsOptions &options, AcceptedWorldSocket accepted);

} // namespace chronicles
