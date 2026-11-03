#include "net/tests/tls_test_support.hpp"
#include "net/atomic_frame_batch.hpp"
#include "net/preparation_workspace.hpp"
#include "net/world_connection.hpp"
#include "net/world_tls_listener.hpp"
#include "world/session/retained_frame_delivery.hpp"

#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/write.hpp>
#include <gtest/gtest.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace chronicles::net_test {
namespace {

template <typename T>
class FailingAllocator {
public:
  using value_type = T;

  explicit FailingAllocator(std::shared_ptr<bool> fail) : fail_(std::move(fail)) {
  }

  template <typename Other>
  FailingAllocator(const FailingAllocator<Other> &other) : fail_(other.fail_) {
  }

  T *allocate(std::size_t count) {
    if (*fail_) {
      throw std::bad_alloc();
    }
    return std::allocator<T>{}.allocate(count);
  }

  void deallocate(T *pointer, std::size_t count) noexcept {
    std::allocator<T>{}.deallocate(pointer, count);
  }

  template <typename Other>
  bool operator==(const FailingAllocator<Other> &other) const noexcept {
    return fail_ == other.fail_;
  }

private:
  template <typename>
  friend class FailingAllocator;
  std::shared_ptr<bool> fail_;
};

TEST(AtomicFrameBatchTest, QueueAllocationFailureRollsBackBeforeReadiness) {
  auto fail = std::make_shared<bool>(false);
  std::vector<int, FailingAllocator<int>> queue{FailingAllocator<int>(fail)};
  queue.reserve(2);
  queue.push_back(7);
  std::vector<int> batch{8, 9};
  bool ready = false;
  *fail = true;
  EXPECT_THROW(append_frame_batch_atomically(queue, batch, [&] {
    ready = true;
  }), std::bad_alloc);
  ASSERT_EQ(queue.size(), 1U);
  EXPECT_EQ(queue.front(), 7);
  EXPECT_FALSE(ready);
}

TEST(PreparationWorkspaceTest, OnlyOneConnectionCanPrepareAndFailureReleasesTheSlot) {
  auto first = PreparationWorkspace::try_acquire();
  ASSERT_TRUE(first);
  EXPECT_FALSE(PreparationWorkspace::try_acquire());

  first.reset();
  EXPECT_THROW(
      [] {
        auto lease = PreparationWorkspace::try_acquire();
        if (!lease) {
          throw std::logic_error("Workspace unexpectedly unavailable");
        }
        throw std::bad_alloc();
      }(),
      std::bad_alloc);
  auto after_failure = PreparationWorkspace::try_acquire();
  EXPECT_TRUE(after_failure);
}

namespace asio = boost::asio;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;
using ClientStream = ssl::stream<tcp::socket>;

Frame server_frame(std::uint16_t opcode, std::size_t body_size = 0) {
  constexpr std::size_t maximum_length = (std::numeric_limits<std::uint16_t>::max)();
  if (body_size > maximum_length - 4) {
    throw std::invalid_argument("Test response exceeds the 16-bit framing limit");
  }

  const auto length = body_size + 4;
  Frame frame;
  frame.bytes.resize(length, 0x35);
  frame.bytes[0] = static_cast<std::uint8_t>(length & 0xff);
  frame.bytes[1] = static_cast<std::uint8_t>(length >> 8);
  frame.bytes[2] = static_cast<std::uint8_t>(opcode & 0xff);
  frame.bytes[3] = static_cast<std::uint8_t>(opcode >> 8);
  return frame;
}

Bytes client_frame(std::uint16_t opcode, std::uint32_t sequence = 0, const std::string &body = {}) {
  constexpr std::size_t maximum_length = (std::numeric_limits<std::uint16_t>::max)();
  if (body.size() > maximum_length - 8) {
    throw std::invalid_argument("Test request exceeds the 16-bit framing limit");
  }
  auto frame = server_frame(opcode, body.size() + 4);
  for (std::size_t index = 0; index < 4; ++index) {
    frame.bytes[4 + index] = static_cast<std::uint8_t>((sequence >> (index * 8)) & 0xff);
  }
  std::copy(body.begin(), body.end(), frame.bytes.begin() + 8);
  return frame.bytes;
}

struct ProbeState {
  unsigned ready_count{};
  unsigned prefix_count{};
  unsigned tail_count{};
  unsigned close_count{};
  bool closed_at_hook{};
  bool initial_cleared{};
  bool tail_cleared{};
  std::size_t sample_size{};
  int pending_credential_bytes{};
  std::vector<char> prefix;
  std::vector<char> tail;
  std::vector<Bytes> frames;
  std::vector<std::string> events;
};

class ProbeConnection;

struct ProbeBehavior {
  bool request_native_tail{};
  bool hold_initial_credential{};
  bool throw_on_close{};
  std::optional<std::uint16_t> close_after_written;
  std::function<void(ProbeConnection &, Frame)> frame_handler;
  std::function<void(ProbeConnection &, std::uint16_t)> written_handler;
};

class ProbeConnection final : public WorldConnection {
public:
  ProbeConnection(tcp::socket socket, std::shared_ptr<ssl::context> context,
                  WorldConnectionOptions options, std::shared_ptr<ProbeState> state,
                  ProbeBehavior behavior)
      : WorldConnection(std::move(socket), std::move(context), options), state_(std::move(state)),
        behavior_(std::move(behavior)) {
  }

  void run() {
    start();
  }

  void finish(std::vector<Frame> frames, bool steady_state = true) {
    complete_request(std::move(frames), steady_state);
  }

  void send_inline(Frame frame) {
    enqueue_frame(std::move(frame));
  }

private:
  void on_transport_ready() override {
    ++state_->ready_count;
    state_->events.push_back("ready");
  }

  void on_initial_credential() override {
    ++state_->prefix_count;
    state_->prefix.assign(credential_prefix().begin(), credential_prefix().end());
    state_->sample_size = credential_sample().size();
    state_->pending_credential_bytes = credential_pending_bytes();
    clear_initial_credential();
    state_->initial_cleared = std::ranges::all_of(credential_prefix(), [](char value) {
      return value == 0;
    });
    if (behavior_.hold_initial_credential) {
      return;
    }
    if (behavior_.request_native_tail) {
      read_native_credential_tail();
      return;
    }
    complete_request({server_frame(5)}, false);
  }

  void on_native_credential_tail() override {
    ++state_->tail_count;
    state_->tail.assign(credential_tail().begin(), credential_tail().end());
    clear_native_credential_tail();
    complete_request({server_frame(5)}, false);
  }

  void on_frame(Frame frame) override {
    state_->frames.push_back(frame.bytes);
    state_->events.push_back("frame:" + std::to_string(frame.opcode()));
    if (behavior_.frame_handler) {
      behavior_.frame_handler(*this, std::move(frame));
      return;
    }
    complete_request({server_frame(100)}, true);
  }

  void on_frame_started(std::uint16_t opcode) override {
    state_->events.push_back("start:" + std::to_string(opcode));
  }

  void on_frame_written(std::uint16_t opcode) override {
    state_->events.push_back("written:" + std::to_string(opcode));
    if (behavior_.close_after_written == opcode) {
      close_on_io();
      return;
    }
    if (behavior_.written_handler) {
      behavior_.written_handler(*this, opcode);
    }
  }

  void on_connection_closed() override {
    ++state_->close_count;
    state_->closed_at_hook = closed();
    state_->tail_cleared = std::ranges::all_of(credential_tail(), [](char value) {
      return value == 0;
    });
    state_->events.push_back("closed");
    if (behavior_.throw_on_close) {
      throw std::bad_alloc();
    }
  }

  const std::shared_ptr<ProbeState> state_;
  const ProbeBehavior behavior_;
};

struct OperationResult {
  bool finished{};
  boost::system::error_code error;
};

struct FrameRead : OperationResult {
  std::array<std::uint8_t, 2> length{};
  Frame frame;
};

class ConnectionHarness {
public:
  explicit ConnectionHarness(ProbeBehavior behavior = {}, WorldConnectionOptions options = {})
      : state(std::make_shared<ProbeState>()),
        client_context_(std::make_shared<ssl::context>(ssl::context::tls_client)) {
    if (options.maximum_queued_frames == 0) {
      options.maximum_queued_frames = 256;
    }
    if (options.maximum_queued_bytes == 0) {
      options.maximum_queued_bytes = 1024 * 1024;
    }
    options.connection_id = 42;
    listener_ = start_world_tls_listener(
        io_, tcp::endpoint(asio::ip::address_v4::loopback(), 0), files_.options(),
        [this, options, behavior = std::move(behavior)](tcp::socket socket,
                                                        std::shared_ptr<ssl::context> context) {
          connection = std::make_shared<ProbeConnection>(std::move(socket), std::move(context),
                                                         options, state, behavior);
          connection->run();
        });
    client_context_->set_verify_mode(ssl::verify_peer);
    client_context_->load_verify_file(files_.options().certificate_file.string());
    client_ = std::make_shared<ClientStream>(io_, *client_context_);
  }

  ~ConnectionHarness() {
    listener_->stop();
    if (connection) {
      connection->request_close();
    }
    disconnect();
    if (!abandon_handlers_) {
      if (io_.stopped()) {
        io_.restart();
      }
      io_.poll();
    }
    connection.reset();
  }

  void abandon_handlers() {
    abandon_handlers_ = true;
    io_.stop();
  }

  bool run_until(const std::function<bool()> &finished) {
    auto expired = std::make_shared<bool>(false);
    asio::steady_timer timer(io_);
    timer.expires_after(std::chrono::seconds(3));
    timer.async_wait([expired](boost::system::error_code error) {
      if (!error) {
        *expired = true;
      }
    });
    while (!finished() && !*expired) {
      if (io_.stopped()) {
        io_.restart();
      }
      io_.run_one();
    }
    timer.cancel();
    return finished();
  }

  void handshake() {
    auto result = std::make_shared<OperationResult>();
    client_->next_layer().async_connect(
        tcp::endpoint(asio::ip::address_v4::loopback(), listener_->port()),
        [client = client_, context = client_context_, result](boost::system::error_code error) {
          if (error) {
            result->error = error;
            result->finished = true;
            return;
          }
          client->async_handshake(
              ssl::stream_base::client,
              [client, context, result](boost::system::error_code handshake_error) {
                result->error = handshake_error;
                result->finished = true;
              });
        });
    require_finished(result);
  }

  void write(Bytes bytes) {
    auto result = std::make_shared<OperationResult>();
    auto payload = std::make_shared<Bytes>(std::move(bytes));
    asio::async_write(*client_, asio::buffer(*payload),
                      [client = client_, context = client_context_, payload,
                       result](boost::system::error_code error, std::size_t) {
                        result->error = error;
                        result->finished = true;
                      });
    require_finished(result);
  }

  Frame read() {
    auto result = std::make_shared<FrameRead>();
    asio::async_read(
        *client_, asio::buffer(result->length),
        [client = client_, context = client_context_, result](boost::system::error_code error,
                                                              std::size_t) {
          if (error) {
            result->error = error;
            result->finished = true;
            return;
          }
          const auto length = static_cast<unsigned>(result->length[0]) |
                              (static_cast<unsigned>(result->length[1]) << 8);
          if (length < 4) {
            result->error = asio::error::invalid_argument;
            result->finished = true;
            return;
          }
          result->frame.bytes.resize(length);
          std::copy(result->length.begin(), result->length.end(), result->frame.bytes.begin());
          asio::async_read(
              *client, asio::buffer(result->frame.bytes.data() + 2, length - 2),
              [client, context, result](boost::system::error_code payload_error, std::size_t) {
                result->error = payload_error;
                result->finished = true;
              });
        });
    require_finished(result);
    return std::move(result->frame);
  }

  void enter() {
    handshake();
    write(Bytes(32, 'p'));
    if (read().opcode() != 5) {
      throw std::runtime_error("The synthetic transport probe did not acknowledge its credential");
    }
  }

  void disconnect() {
    boost::system::error_code ignored;
    client_->next_layer().close(ignored);
  }

  void poll() {
    if (io_.stopped()) {
      io_.restart();
    }
    io_.poll();
  }

  std::shared_ptr<ProbeState> state;
  std::shared_ptr<ProbeConnection> connection;

private:
  void require_finished(const std::shared_ptr<OperationResult> &result) {
    if (!run_until([result] {
          return result->finished;
        })) {
      throw std::runtime_error("Private transport test timed out");
    }
    if (result->error) {
      throw boost::system::system_error(result->error, "Private transport test operation failed");
    }
  }

  TemporaryTlsFiles files_;
  asio::io_context io_;
  std::shared_ptr<ssl::context> client_context_;
  std::shared_ptr<ClientStream> client_;
  std::shared_ptr<WorldTlsListener> listener_;
  bool abandon_handlers_{};
};

TEST(WorldConnectionTest, FragmentedInitialCredentialIsDeliveredOnlyWhenComplete) {
  ConnectionHarness harness;
  harness.handshake();
  harness.write(Bytes(7, 'p'));
  EXPECT_EQ(harness.state->prefix_count, 0);
  harness.write(Bytes(25, 'p'));
  EXPECT_EQ(harness.read().opcode(), 5);
  EXPECT_EQ(harness.state->prefix, std::vector<char>(32, 'p'));
  EXPECT_TRUE(harness.state->initial_cleared);
  EXPECT_EQ(harness.connection->remote_address(), "127.0.0.1");
  EXPECT_EQ(harness.connection->connection_id(), 42);
}

TEST(WorldConnectionTest, NativeTailRemainsAvailableAfterTheDiagnosticPeek) {
  ProbeBehavior behavior;
  behavior.request_native_tail = true;
  WorldConnectionOptions options;
  options.credential_diagnostics = true;
  ConnectionHarness harness(behavior, options);
  harness.handshake();
  Bytes credential(100, 't');
  std::fill_n(credential.begin(), 32, 'p');
  const auto following_frame = client_frame(200);
  credential.insert(credential.end(), following_frame.begin(), following_frame.end());
  harness.write(credential);

  EXPECT_EQ(harness.read().opcode(), 5);
  EXPECT_EQ(harness.read().opcode(), 100);
  EXPECT_EQ(harness.state->prefix_count, 1);
  EXPECT_EQ(harness.state->tail_count, 1);
  EXPECT_EQ(harness.state->tail, std::vector<char>(68, 't'));
  EXPECT_GE(harness.state->sample_size, 32);
  EXPECT_LE(harness.state->sample_size, 100);
  EXPECT_GE(harness.state->pending_credential_bytes, 0);
  ASSERT_EQ(harness.state->frames.size(), 1);
  EXPECT_EQ(harness.state->frames.front(), following_frame);
}

TEST(WorldConnectionTest, PartialTailFailureClearsTheBufferBeforeTheCloseHook) {
  ProbeBehavior behavior;
  behavior.request_native_tail = true;
  ConnectionHarness harness(behavior);
  harness.handshake();
  harness.write(Bytes(32, 'p'));
  ASSERT_TRUE(harness.run_until([&] {
    return harness.state->prefix_count == 1;
  }));
  harness.write(Bytes(9, 't'));
  harness.disconnect();

  ASSERT_TRUE(harness.run_until([&] {
    return harness.state->close_count == 1;
  }));
  EXPECT_TRUE(harness.state->tail_cleared);
  EXPECT_TRUE(harness.state->closed_at_hook);
  EXPECT_EQ(harness.state->tail_count, 0);
}

TEST(WorldConnectionTest, FragmentedLengthAndPayloadProduceTheOriginalCompleteFrame) {
  ConnectionHarness harness;
  harness.enter();
  const auto wire = client_frame(200, 3, "fragmented");
  harness.write(Bytes(wire.begin(), wire.begin() + 1));
  harness.write(Bytes(wire.begin() + 1, wire.begin() + 6));
  EXPECT_TRUE(harness.state->frames.empty());
  harness.write(Bytes(wire.begin() + 6, wire.end()));

  EXPECT_EQ(harness.read().opcode(), 100);
  ASSERT_EQ(harness.state->frames.size(), 1);
  EXPECT_EQ(harness.state->frames.front(), wire);
}

TEST(WorldConnectionTest, LargestSixteenBitFrameIsAssembledWithoutNarrowing) {
  ConnectionHarness harness;
  harness.enter();
  const auto wire = client_frame(200, 7, std::string(65527, 'b'));
  harness.write(wire);

  EXPECT_EQ(harness.read().opcode(), 100);
  ASSERT_EQ(harness.state->frames.size(), 1);
  EXPECT_EQ(harness.state->frames.front(), wire);
}

TEST(WorldConnectionTest, AnIgnoredRequestContinuesWithoutWaitingForANonexistentWrite) {
  ProbeBehavior behavior;
  behavior.frame_handler = [](ProbeConnection &connection, Frame frame) {
    if (frame.opcode() == 200) {
      connection.finish({});
    } else {
      connection.finish({server_frame(101)});
    }
  };
  ConnectionHarness harness(behavior);
  harness.enter();
  auto wire = client_frame(200);
  const auto second = client_frame(201, 1);
  wire.insert(wire.end(), second.begin(), second.end());
  harness.write(wire);

  EXPECT_EQ(harness.read().opcode(), 101);
  EXPECT_EQ(harness.state->frames.size(), 2);
}

TEST(WorldConnectionTest, CoalescedFramesKeepTheirOrderAndWaitForOwnWrites) {
  ProbeBehavior behavior;
  behavior.frame_handler = [](ProbeConnection &connection, Frame frame) {
    if (frame.opcode() == 200) {
      connection.finish({server_frame(100), server_frame(101)});
    } else {
      connection.finish({server_frame(103)});
    }
  };
  behavior.written_handler = [](ProbeConnection &connection, std::uint16_t opcode) {
    if (opcode == 100) {
      connection.send_inline(server_frame(102));
    }
  };
  ConnectionHarness harness(behavior);
  harness.enter();
  auto wire = client_frame(200);
  const auto second = client_frame(201, 1);
  wire.insert(wire.end(), second.begin(), second.end());
  harness.write(wire);

  EXPECT_EQ(harness.read().opcode(), 100);
  EXPECT_EQ(harness.read().opcode(), 101);
  EXPECT_EQ(harness.read().opcode(), 102);
  EXPECT_EQ(harness.read().opcode(), 103);
  EXPECT_EQ(harness.state->frames.size(), 2);
  const auto own_written = std::ranges::find(harness.state->events, "written:101");
  const auto second_received = std::ranges::find(harness.state->events, "frame:201");
  ASSERT_NE(own_written, harness.state->events.end());
  ASSERT_NE(second_received, harness.state->events.end());
  EXPECT_LT(own_written, second_received);
}

TEST(WorldConnectionTest, InvalidClientHeaderClosesWithoutDispatch) {
  ConnectionHarness harness;
  harness.enter();
  harness.write({7, 0});

  ASSERT_TRUE(harness.run_until([&] {
    return harness.state->close_count == 1;
  }));
  EXPECT_TRUE(harness.state->frames.empty());
  EXPECT_TRUE(harness.connection->closed());
}

TEST(WorldConnectionTest, SteadyStateBarrierResumesDuringAContinuousNotificationStream) {
  struct TrafficState {
    unsigned scheduled{};
    unsigned written{};
    std::optional<unsigned> written_at_second_request;
  };
  constexpr unsigned notification_limit = 128;
  auto traffic = std::make_shared<TrafficState>();
  ProbeBehavior behavior;
  behavior.frame_handler = [traffic](ProbeConnection &connection, Frame frame) {
    if (frame.opcode() == 200) {
      connection.finish({server_frame(100)});
    } else {
      traffic->written_at_second_request = traffic->written;
      connection.finish({server_frame(101)});
    }
  };
  behavior.written_handler = [traffic](ProbeConnection &connection, std::uint16_t opcode) {
    if (opcode == 900) {
      ++traffic->written;
    }
    if ((opcode == 100 || opcode == 900) && !traffic->written_at_second_request &&
        traffic->scheduled < notification_limit) {
      ++traffic->scheduled;
      connection.send_inline(server_frame(900));
    }
  };
  ConnectionHarness harness(behavior);
  harness.enter();
  auto wire = client_frame(200);
  const auto second = client_frame(201, 1);
  wire.insert(wire.end(), second.begin(), second.end());
  harness.write(wire);

  ASSERT_TRUE(harness.run_until([traffic] {
    return traffic->written_at_second_request.has_value();
  }));
  EXPECT_GT(traffic->scheduled, 0);
  EXPECT_LT(*traffic->written_at_second_request, notification_limit);
}

TEST(WorldConnectionTest, QueueFrameLimitClosesExactlyOnce) {
  ProbeBehavior behavior;
  behavior.frame_handler = [](ProbeConnection &connection, Frame) {
    connection.finish({server_frame(100), server_frame(101), server_frame(102)});
  };
  WorldConnectionOptions options;
  options.maximum_queued_frames = 1;
  ConnectionHarness harness(behavior, options);
  harness.enter();
  harness.write(client_frame(200));

  ASSERT_TRUE(harness.run_until([&] {
    return harness.state->close_count == 1;
  }));
  harness.connection->request_close();
  harness.poll();
  EXPECT_TRUE(harness.connection->closed());
  EXPECT_EQ(harness.state->close_count, 1);
}

TEST(WorldConnectionTest, QueueByteLimitRejectsTheBatch) {
  ProbeBehavior behavior;
  behavior.frame_handler = [](ProbeConnection &connection, Frame) {
    connection.finish({server_frame(100, 20)});
  };
  WorldConnectionOptions options;
  options.maximum_queued_bytes = 12;
  ConnectionHarness harness(behavior, options);
  harness.enter();
  harness.write(client_frame(200));

  ASSERT_TRUE(harness.run_until([&] {
    return harness.state->close_count == 1;
  }));
  EXPECT_TRUE(harness.state->closed_at_hook);
}

TEST(WorldConnectionTest, WriteCompletionHookCanCloseBeforeTheNextCoalescedRead) {
  ProbeBehavior behavior;
  behavior.close_after_written = 100;
  ConnectionHarness harness(behavior);
  harness.enter();
  auto wire = client_frame(200);
  const auto second = client_frame(201);
  wire.insert(wire.end(), second.begin(), second.end());
  harness.write(wire);

  EXPECT_EQ(harness.read().opcode(), 100);
  ASSERT_TRUE(harness.run_until([&] {
    return harness.state->close_count == 1;
  }));
  EXPECT_EQ(harness.state->frames.size(), 1);
}

TEST(WorldConnectionTest, ExternalThreadPostsAndClosesOnTheIoExecutor) {
  ConnectionHarness harness;
  harness.enter();
  std::thread producer([connection = harness.connection] {
    connection->post_frame(server_frame(901));
  });
  producer.join();
  EXPECT_EQ(std::ranges::find(harness.state->events, "start:901"), harness.state->events.end());
  EXPECT_EQ(harness.read().opcode(), 901);

  std::thread closer([connection = harness.connection] {
    connection->request_close();
  });
  closer.join();
  EXPECT_EQ(harness.state->close_count, 0);
  ASSERT_TRUE(harness.run_until([&] {
    return harness.state->close_count == 1;
  }));
}

TEST(WorldConnectionTest, LocalPreparationDropsOrReplacesBeforeIoEnqueue) {
  ConnectionHarness harness;
  harness.enter();
  auto calls = std::make_shared<int>(0);
  auto stale = server_frame(901);
  stale.prepare_for_enqueue = [calls](Frame &) {
    ++*calls;
    return false;
  };
  auto refreshed = server_frame(902);
  refreshed.prepare_for_enqueue = [calls](Frame &frame) {
    ++*calls;
    frame.bytes = server_frame(903).bytes;
    return true;
  };
  harness.connection->post_frame(std::move(stale));
  harness.connection->post_frame(std::move(refreshed));
  EXPECT_EQ(*calls, 0);
  EXPECT_EQ(harness.read().opcode(), 903);
  EXPECT_EQ(*calls, 2);
  EXPECT_EQ(std::ranges::find(harness.state->events, "start:901"), harness.state->events.end());
  EXPECT_EQ(std::ranges::find(harness.state->events, "start:902"), harness.state->events.end());
}

TEST(WorldConnectionTest, LocalPreparationAlsoGuardsTheActorResponseBatch) {
  ProbeBehavior behavior;
  behavior.frame_handler = [](ProbeConnection &connection, Frame) {
    auto stale = server_frame(901);
    stale.prepare_for_enqueue = [](Frame &) {
      return false;
    };
    connection.finish({std::move(stale), server_frame(903)});
  };
  ConnectionHarness harness(behavior);
  harness.enter();
  harness.write(client_frame(200));
  EXPECT_EQ(harness.read().opcode(), 903);
  EXPECT_EQ(std::ranges::find(harness.state->events, "start:901"), harness.state->events.end());
}

TEST(WorldConnectionTest, LocalPreparationExceptionClosesWithoutSendingTheFrame) {
  ConnectionHarness harness;
  harness.enter();
  auto frame = server_frame(901);
  frame.prepare_for_enqueue = [](Frame &) -> bool {
    throw std::runtime_error("Injected frame preparation failure");
  };
  harness.connection->post_frame(std::move(frame));
  ASSERT_TRUE(harness.run_until([&] {
    return harness.state->close_count == 1;
  }));
  EXPECT_EQ(std::ranges::find(harness.state->events, "start:901"), harness.state->events.end());
}

TEST(WorldConnectionTest, CatchupBatchIsInsertedBeforeItsReadinessMarker) {
  ConnectionHarness harness;
  harness.enter();
  auto ready = std::make_shared<bool>(false);
  Frame marker;
  marker.prepare_for_enqueue = [ready](Frame &frame) {
    frame.prepared_batch = std::make_shared<std::vector<Frame>>();
    frame.prepared_batch->push_back(server_frame(910));
    frame.prepared_batch->push_back(server_frame(911));
    frame.after_enqueue = [ready] {
      *ready = true;
    };
    return true;
  };
  harness.connection->post_frame(std::move(marker));
  EXPECT_EQ(harness.read().opcode(), 910);
  EXPECT_EQ(harness.read().opcode(), 911);
  EXPECT_TRUE(*ready);
}

TEST(WorldConnectionTest, CatchupReadinessFailureRollsBackBeforeAnyWrite) {
  ConnectionHarness harness;
  harness.enter();
  Frame marker;
  marker.prepare_for_enqueue = [](Frame &frame) {
    frame.prepared_batch = std::make_shared<std::vector<Frame>>();
    frame.prepared_batch->push_back(server_frame(910));
    frame.prepared_batch->push_back(server_frame(911));
    frame.after_enqueue = [] {
      throw std::bad_alloc();
    };
    return true;
  };
  harness.connection->post_frame(std::move(marker));
  ASSERT_TRUE(harness.run_until([&] {
    return harness.state->close_count == 1;
  }));
  EXPECT_EQ(std::ranges::find(harness.state->events, "start:910"), harness.state->events.end());
  EXPECT_EQ(std::ranges::find(harness.state->events, "start:911"), harness.state->events.end());
}

TEST(WorldConnectionTest, PostedCompletionReportsActualInsertionOrStaleDropOnce) {
  ConnectionHarness harness;
  harness.enter();
  std::vector<FrameEnqueueStatus> results;
  auto stale = server_frame(910);
  stale.prepare_for_enqueue = [](Frame &) {
    return false;
  };
  harness.connection->post_frame(std::move(stale), [&](FrameEnqueueStatus status) {
    results.push_back(status);
  });
  harness.connection->post_frame(server_frame(911), [&](FrameEnqueueStatus status) {
    results.push_back(status);
  });
  ASSERT_TRUE(harness.run_until([&] {
    return results.size() == 2;
  }));
  EXPECT_EQ(results[0], FrameEnqueueStatus::stale);
  EXPECT_EQ(results[1], FrameEnqueueStatus::inserted);
  EXPECT_EQ(harness.read().opcode(), 911);
  EXPECT_EQ(results.size(), 2U);
}

TEST(WorldConnectionTest, DestroyingAQueuedHandlerCompletesItAsCancelledOnce) {
  auto results = std::make_shared<std::vector<FrameEnqueueStatus>>();
  {
    ConnectionHarness harness;
    harness.enter();
    harness.connection->post_frame(server_frame(911), [results](FrameEnqueueStatus status) {
      results->push_back(status);
    });
    harness.abandon_handlers();
    EXPECT_TRUE(results->empty());
  }
  ASSERT_EQ(results->size(), 1U);
  EXPECT_EQ(results->front(), FrameEnqueueStatus::cancelled);
}

TEST(WorldConnectionTest, PostingToAClosedConnectionReportsClosedOnce) {
  ConnectionHarness harness;
  harness.enter();
  harness.connection->request_close();
  ASSERT_TRUE(harness.run_until([&] {
    return harness.state->close_count == 1;
  }));
  std::vector<FrameEnqueueStatus> results;
  harness.connection->post_frame(server_frame(911), [&](FrameEnqueueStatus status) {
    results.push_back(status);
  });
  ASSERT_EQ(results.size(), 1U);
  EXPECT_EQ(results.front(), FrameEnqueueStatus::closed);
}

TEST(WorldConnectionTest, CloseHookFailureCannotLeaveTheSocketOpen) {
  ProbeBehavior behavior;
  behavior.throw_on_close = true;
  ConnectionHarness harness(behavior);
  harness.enter();
  harness.connection->request_close();
  ASSERT_TRUE(harness.run_until([&] {
    return harness.state->close_count == 1;
  }));
  EXPECT_TRUE(harness.connection->closed());
  EXPECT_THROW(harness.read(), boost::system::system_error);
}

class ThrowingCloseLogSink final : public spdlog::sinks::base_sink<std::mutex> {
protected:
  void sink_it_(const spdlog::details::log_msg &) override {
    throw std::runtime_error("Injected close log sink failure");
  }

  void flush_() override {
  }
};

class ScopedDefaultLogger {
public:
  explicit ScopedDefaultLogger(std::shared_ptr<spdlog::logger> logger)
      : previous_(spdlog::default_logger()) {
    spdlog::set_default_logger(std::move(logger));
  }

  ~ScopedDefaultLogger() {
    spdlog::set_default_logger(std::move(previous_));
  }

private:
  std::shared_ptr<spdlog::logger> previous_;
};

TEST(WorldConnectionTest, CloseHookAndLoggerFailuresDoNotEscapeIo) {
  ProbeBehavior behavior;
  behavior.throw_on_close = true;
  ConnectionHarness harness(behavior);
  harness.enter();

  bool error_handler_called = false;
  auto logger = std::make_shared<spdlog::logger>("throwing-close-test",
                                                 std::make_shared<ThrowingCloseLogSink>());
  logger->set_level(spdlog::level::critical);
  logger->set_error_handler([&error_handler_called](const std::string &) {
    error_handler_called = true;
    throw std::runtime_error("Injected close log error handler failure");
  });
  {
    ScopedDefaultLogger scoped_logger(std::move(logger));
    harness.connection->request_close();
    EXPECT_NO_THROW(ASSERT_TRUE(harness.run_until([&] {
      return harness.state->close_count == 1;
    })));
  }

  EXPECT_TRUE(error_handler_called);
  EXPECT_TRUE(harness.connection->closed());
  EXPECT_THROW(harness.read(), boost::system::system_error);
}

TEST(WorldConnectionTest, TwoTlsRecipientsResolveStaleAndDepartedViewsWithoutReplay) {
  ConnectionHarness actor;
  ConnectionHarness peer;
  actor.enter();
  peer.enter();
  auto revision = std::make_shared<int>(1);
  const GatheringViewIdentity actor_view{1, 9012, 7};
  const GatheringViewIdentity peer_view{1, 9012, 7};
  auto current_peer_view = peer_view;
  auto stale_frame = server_frame(910);
  stale_frame.prepare_for_enqueue = [revision](Frame &) {
    return *revision == 1;
  };
  auto peer_frame = server_frame(912);
  peer_frame.prepare_for_enqueue = [&current_peer_view, peer_view](Frame &) {
    return current_peer_view == peer_view;
  };
  auto delivery = std::make_shared<RetainedFrameDelivery>(
      "event", std::vector<RetainedFrameDelivery::Target>{
                   {10, actor_view, {stale_frame, server_frame(911)}},
                   {11, peer_view, {peer_frame}}});
  auto first = delivery->begin(0);
  ASSERT_TRUE(first);
  actor.connection->post_frame(first->frame, [delivery, attempt = *first](FrameEnqueueStatus status) {
    EXPECT_TRUE(delivery->finish(attempt, status, true));
  });
  *revision = 2;
  ASSERT_TRUE(actor.run_until([&] {
    return delivery->needs_rebuild(0);
  }));
  EXPECT_TRUE(delivery->replace_stale(0, server_frame(913)));
  auto rebuilt = delivery->begin(0);
  ASSERT_TRUE(rebuilt);
  actor.connection->post_frame(rebuilt->frame,
      [delivery, attempt = *rebuilt](FrameEnqueueStatus status) {
        EXPECT_TRUE(delivery->finish(attempt, status, true));
      });
  EXPECT_EQ(actor.read().opcode(), 913);
  std::optional<RetainedFrameDelivery::Attempt> second;
  ASSERT_TRUE(actor.run_until([&] {
    if (!second) {
      second = delivery->begin(0);
    }
    return second.has_value();
  }));
  actor.connection->post_frame(second->frame,
      [delivery, attempt = *second](FrameEnqueueStatus status) {
        EXPECT_TRUE(delivery->finish(attempt, status, true));
      });
  EXPECT_EQ(actor.read().opcode(), 911);

  auto peer_attempt = delivery->begin(1);
  ASSERT_TRUE(peer_attempt);
  peer.connection->post_frame(peer_attempt->frame,
      [delivery, attempt = *peer_attempt, &current_peer_view, peer_view](FrameEnqueueStatus status) {
        EXPECT_TRUE(delivery->finish(attempt, status, current_peer_view == peer_view));
      });
  ++current_peer_view.epoch;
  ASSERT_TRUE(peer.run_until([&] {
    return delivery->complete();
  }));
  EXPECT_EQ(std::ranges::find(actor.state->events, "start:910"), actor.state->events.end());
  EXPECT_EQ(std::ranges::find(peer.state->events, "start:912"), peer.state->events.end());
}

TEST(WorldConnectionTest, StalledIoBoundsPostedFramesAndReleasesTheirPlans) {
  WorldConnectionOptions options;
  options.maximum_posted_frames = 2;
  options.maximum_posted_bytes = 32 * 1024;
  ConnectionHarness harness({}, options);
  harness.enter();

  std::vector<std::weak_ptr<int>> plans;
  for (int index = 0; index < 3; ++index) {
    auto retained = std::make_shared<int>(index);
    plans.push_back(retained);
    auto frame = server_frame(static_cast<std::uint16_t>(910 + index));
    frame.local_retained_bytes = 4096;
    frame.prepare_for_enqueue = [retained](Frame &) {
      return *retained >= 0;
    };
    harness.connection->post_frame(std::move(frame));
  }
  EXPECT_FALSE(plans[0].expired());
  EXPECT_FALSE(plans[1].expired());
  EXPECT_TRUE(plans[2].expired());
  ASSERT_TRUE(harness.run_until([&] {
    return harness.state->close_count == 1;
  }));
  EXPECT_TRUE(plans[0].expired());
  EXPECT_TRUE(plans[1].expired());
}

TEST(WorldConnectionTest, PostedFrameByteBudgetRejectsOversizedMetadata) {
  WorldConnectionOptions options;
  options.maximum_posted_frames = 4;
  options.maximum_posted_bytes = 4096;
  ConnectionHarness harness({}, options);
  harness.enter();
  auto frame = server_frame(901);
  frame.local_retained_bytes = 4096;
  harness.connection->post_frame(std::move(frame));
  ASSERT_TRUE(harness.run_until([&] {
    return harness.state->close_count == 1;
  }));
  EXPECT_EQ(std::ranges::find(harness.state->events, "start:901"), harness.state->events.end());
}

TEST(PendingFrameBudget, CancelledHandlerReleasesCountAndBytes) {
  auto global = std::make_shared<GlobalPendingFrameBudget>(4, 200);
  PendingFrameBudget budget(2, 100, global);
  auto first = budget.acquire(1, 60);
  ASSERT_TRUE(first);
  EXPECT_EQ(budget.acquire(1, 41), nullptr);
  first.reset();
  auto replacement = budget.acquire(2, 100);
  ASSERT_TRUE(replacement);
  EXPECT_EQ(budget.acquire(1, 1), nullptr);
  replacement.reset();
  EXPECT_NE(budget.acquire(1, 1), nullptr);
}

TEST(PendingFrameBudget, GlobalBudgetChargesSharedAllocationOnceAcrossConnections) {
  auto global = std::make_shared<GlobalPendingFrameBudget>(4, 100);
  PendingFrameBudget first(2, 100, global);
  PendingFrameBudget second(2, 100, global);
  PendingFrameBudget third(2, 100, global);
  auto shared = std::make_shared<int>(1);
  const GlobalPendingFrameBudget::Allocation allocation{shared, 60};

  auto first_lease = first.acquire(1, 20, {allocation});
  auto second_lease = second.acquire(1, 20, {allocation});
  ASSERT_TRUE(first_lease);
  ASSERT_TRUE(second_lease);
  EXPECT_EQ(third.acquire(1, 1), nullptr);
  first_lease.reset();
  auto third_lease = third.acquire(1, 20);
  ASSERT_TRUE(third_lease);
  second_lease.reset();
  third_lease.reset();
  EXPECT_NE(third.acquire(1, 100), nullptr);
}

TEST(WorldConnectionTest, SharedWireBytesRemainOwnedThroughWriteCompletion) {
  auto wire = std::make_shared<const Bytes>(server_frame(902, 256).bytes);
  std::weak_ptr<const Bytes> weak_wire = wire;
  auto owned_during_hook = std::make_shared<bool>(false);
  ProbeBehavior behavior;
  behavior.written_handler = [weak_wire, owned_during_hook](ProbeConnection &,
                                                            std::uint16_t opcode) {
    if (opcode == 902) {
      *owned_during_hook = !weak_wire.expired();
    }
  };
  ConnectionHarness harness(behavior);
  harness.enter();
  Frame frame;
  frame.shared = wire;
  harness.connection->post_frame(std::move(frame));
  wire.reset();

  EXPECT_EQ(harness.read().opcode(), 902);
  ASSERT_TRUE(harness.run_until([&] {
    return *owned_during_hook;
  }));
}

TEST(WorldConnectionTest, PendingReadKeepsTheDerivedSessionAliveUntilDisconnect) {
  ConnectionHarness harness;
  harness.enter();
  std::weak_ptr<ProbeConnection> pending = harness.connection;
  harness.connection.reset();
  EXPECT_FALSE(pending.expired());
  harness.disconnect();

  ASSERT_TRUE(harness.run_until([&] {
    return pending.expired();
  }));
  EXPECT_EQ(harness.state->close_count, 1);
}

TEST(WorldConnectionTest, DiagnosticDelayRemainsOutsideTheTlsHandshake) {
  WorldConnectionOptions options;
  options.handshake_delay = std::chrono::milliseconds(100);
  ConnectionHarness harness({}, options);
  const auto started = std::chrono::steady_clock::now();
  harness.handshake();
  const auto elapsed = std::chrono::steady_clock::now() - started;

  EXPECT_GE(elapsed, std::chrono::milliseconds(80));
  EXPECT_EQ(harness.state->ready_count, 1);
  EXPECT_EQ(harness.state->prefix_count, 0);
}

TEST(WorldConnectionTest, ConstructorRejectsMissingContextAndInvalidTransportBounds) {
  asio::io_context context;
  auto state = std::make_shared<ProbeState>();
  auto tls = std::make_shared<ssl::context>(ssl::context::tls_server);
  WorldConnectionOptions options;
  options.maximum_queued_frames = 1;
  options.maximum_queued_bytes = 1024;
  const auto construct = [&](const std::shared_ptr<ssl::context> &selected_context) {
    static_cast<void>(std::make_shared<ProbeConnection>(tcp::socket(context), selected_context,
                                                        options, state, ProbeBehavior{}));
  };
  EXPECT_THROW(construct(nullptr), std::invalid_argument);

  options.maximum_queued_frames = 0;
  EXPECT_THROW(construct(tls), std::invalid_argument);
  options.maximum_queued_frames = 1;
  options.maximum_queued_bytes = 0;
  EXPECT_THROW(construct(tls), std::invalid_argument);
  options.maximum_queued_bytes = 1024;
  options.handshake_delay = std::chrono::milliseconds(-1);
  EXPECT_THROW(construct(tls), std::invalid_argument);
}

} // namespace
} // namespace chronicles::net_test
