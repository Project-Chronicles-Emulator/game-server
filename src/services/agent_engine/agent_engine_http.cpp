#include "services/agent_engine/agent_engine_http.hpp"

#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <spdlog/spdlog.h>

#include <chrono>

namespace chronicles::agent_engine {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

constexpr auto kReadTimeout = std::chrono::seconds(30);
// Covers a slow language-model reply plus writing the response.
constexpr auto kWriteTimeout = std::chrono::seconds(60);
constexpr std::uint32_t kMaximumHeaderBytes = 16 * 1024;

http::response<http::string_body> to_http_response(NativeHttpResponse result) {
  http::response<http::string_body> response{static_cast<http::status>(result.status), 11};
  response.set(http::field::content_type, result.content_type);
  response.set(http::field::cache_control, "no-store");
  response.keep_alive(false);
  response.body() = std::move(result.body);
  response.prepare_payload();
  return response;
}

class Session : public std::enable_shared_from_this<Session> {
public:
  Session(tcp::socket socket, AgentEngineRest &rest, asio::thread_pool &workers, bool allow_remote,
          std::shared_ptr<std::atomic_uint> connections)
      : stream_(std::move(socket)), rest_(rest), workers_(workers), allow_remote_(allow_remote),
        connections_(std::move(connections)) {
    parser_.body_limit(kMaximumRequestBodyBytes);
    parser_.header_limit(kMaximumHeaderBytes);
  }

  ~Session() {
    --*connections_;
  }

  void run() {
    stream_.expires_after(kReadTimeout);
    http::async_read(stream_, buffer_, parser_,
                     [self = shared_from_this()](beast::error_code error, std::size_t) {
                       self->on_read(error);
                     });
  }

private:
  void on_read(beast::error_code error) {
    if (error == http::error::body_limit) {
      write(to_http_response({413, "application/json", "{\"status\":\"body_too_large\"}"}));
      return;
    }
    if (error) {
      return;
    }

    beast::error_code endpoint_error;
    const auto peer = stream_.socket().remote_endpoint(endpoint_error);
    const bool loopback = !endpoint_error && peer.address().is_loopback();
    if (!allow_remote_ && !loopback) {
      write(to_http_response({403, "application/json", "{\"status\":\"loopback_only\"}"}));
      return;
    }

    auto request = parser_.release();
    asio::post(workers_, [self = shared_from_this(), request = std::move(request)] {
      const auto method = request.method_string();
      const auto target = request.target();
      const auto content_type = request[http::field::content_type];
      auto result = self->rest_.handle(std::string_view(method.data(), method.size()),
                                       std::string_view(target.data(), target.size()),
                                       std::string_view(content_type.data(), content_type.size()),
                                       request.body());
      asio::post(self->stream_.get_executor(), [self, result = std::move(result)]() mutable {
        self->write(to_http_response(std::move(result)));
      });
    });
  }

  void write(http::response<http::string_body> response) {
    response_ = std::move(response);
    stream_.expires_after(kWriteTimeout);
    http::async_write(stream_, response_,
                      [self = shared_from_this()](beast::error_code, std::size_t) {
                        beast::error_code ignored;
                        self->stream_.socket().shutdown(tcp::socket::shutdown_send, ignored);
                      });
  }

  beast::tcp_stream stream_;
  beast::flat_buffer buffer_;
  http::request_parser<http::string_body> parser_;
  http::response<http::string_body> response_;
  AgentEngineRest &rest_;
  asio::thread_pool &workers_;
  bool allow_remote_{};
  std::shared_ptr<std::atomic_uint> connections_;
};

} // namespace

AgentEngineListener::AgentEngineListener(asio::io_context &io, const tcp::endpoint &endpoint,
                                         AgentEngineRest &rest, asio::thread_pool &workers,
                                         bool allow_remote)
    : acceptor_(io), rest_(rest), workers_(workers), allow_remote_(allow_remote),
      connections_(std::make_shared<std::atomic_uint>(0)) {
  acceptor_.open(endpoint.protocol());
  acceptor_.set_option(asio::socket_base::reuse_address(true));
  acceptor_.bind(endpoint);
  acceptor_.listen();
}

unsigned short AgentEngineListener::port() const {
  return acceptor_.local_endpoint().port();
}

void AgentEngineListener::run() {
  accept_next();
}

void AgentEngineListener::accept_next() {
  acceptor_.async_accept(
      asio::make_strand(acceptor_.get_executor()),
      [self = shared_from_this()](beast::error_code error, tcp::socket socket) {
        if (!error) {
          if (self->connections_->fetch_add(1) < kMaximumAgentEngineConnections) {
            std::make_shared<Session>(std::move(socket), self->rest_, self->workers_,
                                      self->allow_remote_, self->connections_)
                ->run();
          } else {
            --*self->connections_;
          }
        }
        if (self->acceptor_.is_open()) {
          self->accept_next();
        }
      });
}

} // namespace chronicles::agent_engine
