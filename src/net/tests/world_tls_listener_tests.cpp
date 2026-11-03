#include "net/tests/tls_test_support.hpp"
#include "net/world_tls_listener.hpp"

#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/system_error.hpp>
#include <gtest/gtest.h>
#include <openssl/ssl.h>

#include <chrono>
#include <fstream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace chronicles {
namespace {

namespace asio = boost::asio;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;
using net_test::TemporaryTlsFiles;

struct AcceptedConnection {
  unsigned count{};
  bool socket_open{};
  std::shared_ptr<ssl::context> context;
  std::unique_ptr<tcp::socket> socket;
};

struct ListenerOwner {
  std::shared_ptr<WorldTlsListener> listener;
};

struct HandshakeResult {
  bool server_finished{};
  bool client_finished{};
  boost::system::error_code server_error;
  boost::system::error_code client_error;
};

class WorldTlsListenerTest : public testing::Test {
protected:
  void start(AcceptedWorldSocket accepted) {
    listener_ = start_world_tls_listener(io_context_, loopback(), tls_files_.options(),
                                         std::move(accepted));
  }

  static tcp::endpoint loopback(unsigned short port = 0) {
    return {asio::ip::address_v4::loopback(), port};
  }

  tcp::socket connect_client() {
    tcp::socket client(io_context_);
    client.connect(loopback(listener_->port()));
    return client;
  }

  bool run_until(const std::function<bool()> &finished) {
    auto expired = std::make_shared<bool>(false);
    asio::steady_timer deadline(io_context_);
    deadline.expires_after(std::chrono::seconds(3));
    deadline.async_wait([expired](boost::system::error_code error) {
      if (!error) {
        *expired = true;
      }
    });

    while (!finished() && !*expired) {
      if (io_context_.stopped()) {
        io_context_.restart();
      }
      io_context_.run_one();
    }
    deadline.cancel();
    return finished();
  }

  void TearDown() override {
    if (listener_) {
      listener_->stop();
    }
    if (io_context_.stopped()) {
      io_context_.restart();
    }
    io_context_.poll();
  }

  TemporaryTlsFiles tls_files_;
  asio::io_context io_context_;
  std::shared_ptr<WorldTlsListener> listener_;
};

TEST_F(WorldTlsListenerTest, BindsAnEphemeralLoopbackPortAndStopsIdempotently) {
  start([](tcp::socket, std::shared_ptr<ssl::context>) {
  });

  EXPECT_NE(listener_->port(), 0);
  EXPECT_TRUE(listener_->is_open());

  listener_->stop();
  listener_->stop();
  EXPECT_FALSE(listener_->is_open());
}

TEST_F(WorldTlsListenerTest, HandsOffASocketAndTheConfiguredSharedContext) {
  auto accepted = std::make_shared<AcceptedConnection>();
  start([accepted](tcp::socket socket, std::shared_ptr<ssl::context> context) {
    ++accepted->count;
    accepted->socket_open = socket.is_open();
    accepted->context = std::move(context);
  });

  auto client = connect_client();
  ASSERT_TRUE(run_until([accepted] {
    return accepted->count == 1;
  }));
  ASSERT_TRUE(accepted->context);
  EXPECT_TRUE(accepted->socket_open);
  EXPECT_EQ(SSL_CTX_check_private_key(accepted->context->native_handle()), 1);

  const auto disabled_versions =
      SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1;
  const auto options = SSL_CTX_get_options(accepted->context->native_handle());
  EXPECT_EQ(options & disabled_versions, disabled_versions);
  EXPECT_EQ(SSL_CTX_get_max_proto_version(accepted->context->native_handle()), 0);
}

TEST_F(WorldTlsListenerTest, AppliesTheOptionalTls12Maximum) {
  auto options = tls_files_.options();
  options.tls12_only = true;
  auto accepted = std::make_shared<AcceptedConnection>();
  listener_ =
      start_world_tls_listener(io_context_, loopback(), options,
                               [accepted](tcp::socket, std::shared_ptr<ssl::context> context) {
                                 accepted->context = std::move(context);
                               });

  auto client = connect_client();
  ASSERT_TRUE(run_until([accepted] {
    return static_cast<bool>(accepted->context);
  }));
  EXPECT_EQ(SSL_CTX_get_max_proto_version(accepted->context->native_handle()), TLS1_2_VERSION);
}

TEST_F(WorldTlsListenerTest, AThrowingFactoryDoesNotPreventTheNextAccept) {
  auto accepted = std::make_shared<AcceptedConnection>();
  start([accepted](tcp::socket, std::shared_ptr<ssl::context>) {
    ++accepted->count;
    if (accepted->count == 1) {
      throw std::runtime_error("Synthetic session factory failure");
    }
  });

  auto first_client = connect_client();
  ASSERT_TRUE(run_until([accepted] {
    return accepted->count == 1;
  }));
  auto second_client = connect_client();
  ASSERT_TRUE(run_until([accepted] {
    return accepted->count == 2;
  }));
  EXPECT_TRUE(listener_->is_open());
}

TEST_F(WorldTlsListenerTest, StopCancelsAPendingAcceptWithoutCallingTheFactory) {
  auto accepted = std::make_shared<AcceptedConnection>();
  start([accepted](tcp::socket, std::shared_ptr<ssl::context>) {
    ++accepted->count;
  });
  const auto port = listener_->port();
  std::weak_ptr<WorldTlsListener> pending_listener = listener_;

  listener_->stop();
  listener_.reset();
  io_context_.run();

  EXPECT_EQ(accepted->count, 0);
  EXPECT_TRUE(pending_listener.expired());
  tcp::socket client(io_context_);
  boost::system::error_code error;
  client.connect(loopback(port), error);
  EXPECT_TRUE(error);
}

TEST_F(WorldTlsListenerTest, StopFromTheFactoryDoesNotScheduleAnotherAccept) {
  auto accepted = std::make_shared<AcceptedConnection>();
  start([this, accepted](tcp::socket, std::shared_ptr<ssl::context>) {
    ++accepted->count;
    listener_->stop();
  });

  auto client = connect_client();
  ASSERT_TRUE(run_until([accepted] {
    return accepted->count == 1;
  }));
  EXPECT_FALSE(listener_->is_open());

  std::weak_ptr<WorldTlsListener> completed_listener = listener_;
  listener_.reset();
  io_context_.poll();
  EXPECT_TRUE(completed_listener.expired());
}

TEST_F(WorldTlsListenerTest, AnAcceptedContextAndSocketOutliveTheStoppedListener) {
  auto accepted = std::make_shared<AcceptedConnection>();
  start([accepted](tcp::socket socket, std::shared_ptr<ssl::context> context) {
    accepted->socket = std::make_unique<tcp::socket>(std::move(socket));
    accepted->context = std::move(context);
  });

  auto client_socket = connect_client();
  ASSERT_TRUE(run_until([accepted] {
    return static_cast<bool>(accepted->socket);
  }));
  listener_->stop();
  std::weak_ptr<WorldTlsListener> stopped_listener = listener_;
  listener_.reset();
  io_context_.poll();
  EXPECT_TRUE(stopped_listener.expired());

  auto client_context = std::make_shared<ssl::context>(ssl::context::tls_client);
  client_context->set_verify_mode(ssl::verify_peer);
  client_context->load_verify_file(tls_files_.options().certificate_file.string());
  auto server =
      std::make_shared<ssl::stream<tcp::socket>>(std::move(*accepted->socket), *accepted->context);
  auto client =
      std::make_shared<ssl::stream<tcp::socket>>(std::move(client_socket), *client_context);
  auto result = std::make_shared<HandshakeResult>();

  // Completions retain streams, contexts and result state, including a failed timeout path.
  server->async_handshake(ssl::stream_base::server,
                          [server, accepted, result](boost::system::error_code error) {
                            result->server_error = error;
                            result->server_finished = true;
                          });
  client->async_handshake(ssl::stream_base::client,
                          [client, client_context, result](boost::system::error_code error) {
                            result->client_error = error;
                            result->client_finished = true;
                          });

  const bool finished = run_until([result] {
    return result->server_finished && result->client_finished;
  });
  if (!finished) {
    boost::system::error_code ignored;
    server->next_layer().close(ignored);
    client->next_layer().close(ignored);
    io_context_.poll();
  }

  ASSERT_TRUE(finished);
  EXPECT_FALSE(result->server_error) << result->server_error.message();
  EXPECT_FALSE(result->client_error) << result->client_error.message();
  EXPECT_EQ(SSL_get_verify_result(client->native_handle()), X509_V_OK);
}

TEST_F(WorldTlsListenerTest, PendingAcceptRetainsItsCallbackOwnerUntilStopIsDrained) {
  auto owner = std::make_shared<ListenerOwner>();
  std::weak_ptr<ListenerOwner> weak_owner = owner;
  owner->listener = start_world_tls_listener(io_context_, loopback(), tls_files_.options(),
                                             [owner](tcp::socket, std::shared_ptr<ssl::context>) {
                                             });
  std::weak_ptr<WorldTlsListener> weak_listener = owner->listener;
  owner.reset();

  ASSERT_FALSE(weak_owner.expired());
  {
    const auto pending_owner = weak_owner.lock();
    pending_owner->listener->stop();
  }
  io_context_.run();

  EXPECT_TRUE(weak_owner.expired());
  EXPECT_TRUE(weak_listener.expired());
}

TEST_F(WorldTlsListenerTest, CallbackOwnerIsReleasedWhenItsIoContextIsDestroyed) {
  auto context = std::make_unique<asio::io_context>();
  auto owner = std::make_shared<ListenerOwner>();
  std::weak_ptr<ListenerOwner> weak_owner = owner;
  owner->listener = start_world_tls_listener(*context, loopback(), tls_files_.options(),
                                             [owner](tcp::socket, std::shared_ptr<ssl::context>) {
                                             });
  std::weak_ptr<WorldTlsListener> weak_listener = owner->listener;
  owner.reset();

  ASSERT_FALSE(weak_owner.expired());
  context.reset();

  EXPECT_TRUE(weak_owner.expired());
  EXPECT_TRUE(weak_listener.expired());
}

TEST_F(WorldTlsListenerTest, RejectsAnEndpointReservedExclusivelyByAnotherAcceptor) {
  tcp::acceptor occupied(io_context_);
  occupied.open(tcp::v4());
#ifdef _WIN32
  // Asio's endpoint constructor enables address reuse. Windows needs an exclusive fixture
  // to make a second bind deterministically fail without changing the listener's policy.
  const int exclusive = 1;
  const auto option_result =
      ::setsockopt(occupied.native_handle(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char *>(&exclusive), static_cast<int>(sizeof(exclusive)));
  if (option_result == SOCKET_ERROR) {
    const boost::system::error_code error(::WSAGetLastError(), boost::system::system_category());
    throw boost::system::system_error(error, "Cannot reserve the test endpoint exclusively");
  }
#else
  occupied.set_option(tcp::acceptor::reuse_address(false));
#endif
  occupied.bind(loopback());
  occupied.listen();

  EXPECT_THROW(start_world_tls_listener(io_context_, occupied.local_endpoint(),
                                        tls_files_.options(),
                                        [](tcp::socket, std::shared_ptr<ssl::context>) {
                                        }),
               boost::system::system_error);
}

TEST_F(WorldTlsListenerTest, RejectsAMissingCertificateAtStartup) {
  auto options = tls_files_.options();
  options.certificate_file = tls_files_.directory() / "missing-certificate.pem";

  EXPECT_THROW(start_world_tls_listener(io_context_, loopback(), options,
                                        [](tcp::socket, std::shared_ptr<ssl::context>) {
                                        }),
               boost::system::system_error);
}

TEST_F(WorldTlsListenerTest, RejectsAnInvalidCertificateAtStartup) {
  auto options = tls_files_.options();
  options.certificate_file = tls_files_.directory() / "invalid-certificate.pem";
  {
    std::ofstream invalid_file(options.certificate_file);
    invalid_file << "This is deliberately not a PEM certificate.\n";
    if (!invalid_file) {
      throw std::runtime_error("Cannot write an invalid certificate test fixture");
    }
  }

  EXPECT_THROW(start_world_tls_listener(io_context_, loopback(), options,
                                        [](tcp::socket, std::shared_ptr<ssl::context>) {
                                        }),
               boost::system::system_error);
}

TEST_F(WorldTlsListenerTest, RejectsAMissingPrivateKeyAtStartup) {
  auto options = tls_files_.options();
  options.private_key_file = tls_files_.directory() / "missing-private-key.pem";

  EXPECT_THROW(start_world_tls_listener(io_context_, loopback(), options,
                                        [](tcp::socket, std::shared_ptr<ssl::context>) {
                                        }),
               boost::system::system_error);
}

TEST_F(WorldTlsListenerTest, RejectsAMismatchedCertificateAndPrivateKeyAtStartup) {
  const TemporaryTlsFiles other_identity;
  auto options = tls_files_.options();
  options.private_key_file = other_identity.options().private_key_file;

  // OpenSSL may reject the key when loading it, before the explicit matching check.
  EXPECT_THROW(start_world_tls_listener(io_context_, loopback(), options,
                                        [](tcp::socket, std::shared_ptr<ssl::context>) {
                                        }),
               std::exception);
}

TEST_F(WorldTlsListenerTest, RejectsAnEmptyFactoryBeforeSchedulingAcceptance) {
  EXPECT_THROW(start_world_tls_listener(io_context_, loopback(), tls_files_.options(), {}),
               std::invalid_argument);
  EXPECT_EQ(io_context_.poll(), 0);
}

} // namespace
} // namespace chronicles
