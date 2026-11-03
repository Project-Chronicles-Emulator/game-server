#include "net/world_tls_listener.hpp"

#include <boost/asio/error.hpp>
#include <openssl/ssl.h>
#include <spdlog/spdlog.h>

#include <stdexcept>
#include <utility>

namespace chronicles {
namespace {

namespace asio = boost::asio;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

class TlsListener final : public WorldTlsListener,
                          public std::enable_shared_from_this<TlsListener> {
public:
  TlsListener(asio::io_context &io_context, const tcp::endpoint &endpoint,
              const WorldTlsOptions &options)
      : acceptor_(io_context, endpoint),
        tls_context_(std::make_shared<ssl::context>(ssl::context::tls_server)) {
    tls_context_->set_options(ssl::context::default_workarounds | ssl::context::no_sslv2 |
                              ssl::context::no_sslv3 | ssl::context::no_tlsv1 |
                              ssl::context::no_tlsv1_1);
    if (options.tls12_only &&
        SSL_CTX_set_max_proto_version(tls_context_->native_handle(), TLS1_2_VERSION) != 1) {
      throw std::runtime_error("Failed to limit world TLS to version 1.2");
    }

    tls_context_->use_certificate_chain_file(options.certificate_file.string());
    tls_context_->use_private_key_file(options.private_key_file.string(), ssl::context::pem);
    if (SSL_CTX_check_private_key(tls_context_->native_handle()) != 1) {
      throw std::runtime_error("World TLS certificate and private key do not match");
    }
  }

  unsigned short port() const override {
    return acceptor_.local_endpoint().port();
  }

  bool is_open() const override {
    return acceptor_.is_open();
  }

  void stop() override {
    boost::system::error_code ignored;
    acceptor_.close(ignored);
  }

  void run(AcceptedWorldSocket accepted) {
    // Retain the factory only through the pending operation, as a factory may own
    // the world facade that owns this listener. A member would form a permanent cycle.
    acceptor_.async_accept([self = shared_from_this(), accepted = std::move(accepted)](
                               boost::system::error_code error, tcp::socket socket) mutable {
      if (!error) {
        try {
          accepted(std::move(socket), self->tls_context_);
        } catch (const std::exception &exception) {
          spdlog::warn("Native world session creation failed: {}", exception.what());
        }
      } else if (error != asio::error::operation_aborted) {
        spdlog::warn("Native world accept failed: {}", error.message());
      }

      if (self->acceptor_.is_open()) {
        self->run(std::move(accepted));
      }
    });
  }

private:
  tcp::acceptor acceptor_;
  std::shared_ptr<ssl::context> tls_context_;
};

} // namespace

std::shared_ptr<WorldTlsListener> start_world_tls_listener(asio::io_context &io_context,
                                                           const tcp::endpoint &endpoint,
                                                           const WorldTlsOptions &options,
                                                           AcceptedWorldSocket accepted) {
  if (!accepted) {
    throw std::invalid_argument("World TLS listener requires an accept handler");
  }

  auto listener = std::make_shared<TlsListener>(io_context, endpoint, options);
  listener->run(std::move(accepted));
  return listener;
}

} // namespace chronicles
