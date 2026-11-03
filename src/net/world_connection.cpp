#include "net/world_connection.hpp"

#include "common/world_metrics.hpp"
#include "net/atomic_frame_batch.hpp"
#include "net/preparation_workspace.hpp"

#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace chronicles {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

constexpr auto kHandshakeTimeout = std::chrono::seconds(15);
constexpr auto kCredentialTailTimeout = std::chrono::seconds(2);
constexpr auto kPacketTimeout = std::chrono::seconds(90);
constexpr std::size_t kClientHeaderBytes = 8;
// A queued handler can retain decoded sector JSON and a source plan in addition to wire bytes.
constexpr std::size_t kDefaultPostedBytes = 32 * 1024 * 1024;
constexpr std::size_t kGlobalPostedBytes = 192 * 1024 * 1024;
constexpr std::size_t kGlobalPostedFrames = 65536;
constexpr std::size_t kPostedFrameOverhead = 1024;

std::shared_ptr<GlobalPendingFrameBudget> default_global_posted_budget() {
  static auto budget =
      std::make_shared<GlobalPendingFrameBudget>(kGlobalPostedFrames, kGlobalPostedBytes);
  return budget;
}

class FramePostCompletion final {
public:
  explicit FramePostCompletion(std::function<void(FrameEnqueueStatus)> callback)
      : callback_(std::move(callback)) {
  }

  ~FramePostCompletion() {
    finish(FrameEnqueueStatus::cancelled);
  }

  void finish(FrameEnqueueStatus status) noexcept {
    if (finished_.exchange(true)) {
      return;
    }
    try {
      callback_(status);
    } catch (...) {
      // The retained owner must also have a timeout; a throwing callback cannot be retried
      // safely here because it may have performed part of its own work.
      spdlog::error("World frame completion callback failed");
    }
  }

private:
  std::function<void(FrameEnqueueStatus)> callback_;
  std::atomic<bool> finished_{};
};

ssl::context &require_tls_context(const std::shared_ptr<ssl::context> &context) {
  if (!context) {
    throw std::invalid_argument("World transport requires a TLS context");
  }
  return *context;
}

} // namespace

WorldConnection::WorldConnection(tcp::socket socket, std::shared_ptr<ssl::context> tls_context,
                                 WorldConnectionOptions options)
    : options_(options),
      posted_frame_budget_(options.maximum_posted_frames != 0 ? options.maximum_posted_frames
                                                              : options.maximum_queued_frames,
                           options.maximum_posted_bytes != 0 ? options.maximum_posted_bytes
                                                             : kDefaultPostedBytes,
                           options.shared_posted_budget ? options.shared_posted_budget
                                                        : default_global_posted_budget()),
      tls_context_(std::move(tls_context)),
      stream_(std::move(socket), require_tls_context(tls_context_)),
      handshake_delay_timer_(stream_.get_executor()) {
  if (options_.maximum_queued_frames == 0 || options_.maximum_queued_bytes == 0 ||
      options_.handshake_delay < std::chrono::milliseconds::zero()) {
    throw std::invalid_argument("World transport requires positive queue limits and a valid delay");
  }

  tls_diagnostics_.connection_id = options_.connection_id;
  boost::system::error_code endpoint_error;
  const auto remote = beast::get_lowest_layer(stream_).socket().remote_endpoint(endpoint_error);
  if (!endpoint_error) {
    remote_address_ = remote.address().to_string();
  }
}

std::shared_ptr<PendingFrameBudget::Lease>
WorldConnection::reserve_posted_frames(std::span<const Frame> frames) {
  const auto maximum_frames = options_.maximum_posted_frames != 0
                                  ? options_.maximum_posted_frames
                                  : options_.maximum_queued_frames;
  if (frames.size() > maximum_frames ||
      frames.size() > (std::numeric_limits<std::size_t>::max)() / 2) {
    return {};
  }
  std::vector<GlobalPendingFrameBudget::Allocation> allocations;
  allocations.reserve(frames.size() * 2);
  std::size_t private_bytes = kPostedFrameOverhead;
  for (const auto &frame : frames) {
    if (frame.local_plan || frame.local_message) {
      return {};
    }
    const auto maximum = (std::numeric_limits<std::size_t>::max)();
    if (private_bytes > maximum - kPostedFrameOverhead) {
      return {};
    }
    private_bytes += kPostedFrameOverhead;
    if (frame.shared) {
      allocations.push_back({frame.shared, frame.shared->size()});
    } else {
      if (frame.bytes.size() > maximum - private_bytes) {
        return {};
      }
      private_bytes += frame.bytes.size();
    }
    if (frame.local_allocation) {
      if (frame.local_allocation->bytes != frame.local_retained_bytes) {
        return {};
      }
      allocations.push_back({frame.local_allocation, frame.local_retained_bytes});
    } else {
      if (frame.local_retained_bytes > maximum - private_bytes) {
        return {};
      }
      private_bytes += frame.local_retained_bytes;
    }
  }
  return posted_frame_budget_.acquire(std::max<std::size_t>(1, frames.size()), private_bytes,
                                       std::move(allocations));
}

void WorldConnection::post_frame(Frame frame, std::function<void(FrameEnqueueStatus)> completion) {
  std::shared_ptr<FramePostCompletion> completion_owner;
  if (completion) {
    try {
      completion_owner = std::make_shared<FramePostCompletion>(completion);
    } catch (...) {
      try {
        completion(FrameEnqueueStatus::failed);
      } catch (...) {
        spdlog::error("World frame completion callback failed");
      }
      return;
    }
  }
  if (closed() || posted_frame_overflow_.load()) {
    if (completion_owner) {
      completion_owner->finish(FrameEnqueueStatus::closed);
    }
    return;
  }
  try {
    if (frame.local_plan || frame.local_message) {
      throw std::logic_error("Cannot post an unbound local frame plan");
    }
    auto lease = reserve_posted_frames(std::span<const Frame>(&frame, 1));
    if (!lease) {
      if (!posted_frame_overflow_.exchange(true)) {
        spdlog::warn("Native world connection={} exceeded pending I/O frame budget", connection_id());
        request_close();
      }
      if (completion_owner) {
        completion_owner->finish(FrameEnqueueStatus::failed);
      }
      return;
    }
    asio::post(io_executor(), [self = shared_from_this(), frame = std::move(frame),
                               lease = std::move(lease), completion_owner]() mutable {
      const auto status = self->enqueue_frame(std::move(frame));
      lease.reset();
      if (completion_owner) {
        completion_owner->finish(status);
      }
    });
  } catch (const std::exception &error) {
    spdlog::error("Could not schedule world frame connection={}: {}", connection_id(), error.what());
    posted_frame_overflow_.store(true);
    if (completion_owner) {
      completion_owner->finish(FrameEnqueueStatus::failed);
    }
    try {
      request_close();
    } catch (...) {
      std::terminate();
    }
  } catch (...) {
    spdlog::error("Could not schedule world frame connection={}: unknown error", connection_id());
    posted_frame_overflow_.store(true);
    if (completion_owner) {
      completion_owner->finish(FrameEnqueueStatus::failed);
    }
    try {
      request_close();
    } catch (...) {
      std::terminate();
    }
  }
}

void WorldConnection::request_close() {
  asio::post(io_executor(), [self = shared_from_this()] {
    self->close_on_io();
  });
}

bool WorldConnection::closed() const noexcept {
  return closed_.load();
}

std::uint64_t WorldConnection::connection_id() const noexcept {
  return options_.connection_id;
}

const std::string &WorldConnection::remote_address() const noexcept {
  return remote_address_;
}

asio::any_io_executor WorldConnection::io_executor() {
  return stream_.get_executor();
}

void WorldConnection::start() {
  attach_world_tls_diagnostics(stream_.native_handle(), tls_diagnostics_);
  disable_nagle_delay();
  on_transport_ready();

  if (options_.handshake_delay == std::chrono::milliseconds::zero()) {
    begin_tls_handshake();
    return;
  }

  spdlog::info("World TLS connection={} diagnostic_handshake_delay_ms={}", connection_id(),
               options_.handshake_delay.count());
  // Keep the pending connection observable without blocking I/O. The normal handshake
  // timeout starts only after this diagnostic delay.
  handshake_delay_timer_.expires_after(options_.handshake_delay);
  handshake_delay_timer_.async_wait([self = shared_from_this()](boost::system::error_code error) {
    if (error) {
      self->fail("TLS diagnostic delay", error);
      return;
    }
    self->begin_tls_handshake();
  });
}

void WorldConnection::on_transport_ready() {
}

void WorldConnection::on_frame_started(std::uint16_t) {
}

void WorldConnection::disable_nagle_delay() {
  // World responses are small. Waiting for acknowledgement of an earlier frame can
  // otherwise delay a Pong, movement notification or skill result by one round trip.
  boost::system::error_code error;
  beast::get_lowest_layer(stream_).socket().set_option(tcp::no_delay(true), error);
  if (error) {
    spdlog::warn("World connection={} could not disable Nagle delay: {}", connection_id(),
                 error.message());
  }
}

void WorldConnection::begin_tls_handshake() {
  beast::get_lowest_layer(stream_).expires_after(kHandshakeTimeout);
  stream_.async_handshake(
      ssl::stream_base::server, [self = shared_from_this()](boost::system::error_code error) {
        log_world_tls_result(self->stream_.native_handle(), self->tls_diagnostics_, error);
        if (error) {
          self->fail("TLS handshake", error);
          return;
        }

        spdlog::info("World TLS negotiation completed; reading prototype credential");
        self->read_packet_key();
      });
}

void WorldConnection::read_packet_key() {
  beast::get_lowest_layer(stream_).expires_after(kHandshakeTimeout);
  asio::async_read(
      stream_, asio::buffer(packet_key_),
      [self = shared_from_this()](boost::system::error_code error, std::size_t bytes_received) {
        if (error) {
          spdlog::warn("World credential read connection={} ended after {} of {} "
                       "bytes: {}",
                       self->connection_id(), bytes_received, kInitialCredentialBytes,
                       error.message());
          self->fail("world credential read", error);
          return;
        }

        if (self->options_.credential_diagnostics) {
          self->capture_credential_sample();
        }
        self->on_initial_credential();
      });
}

void WorldConnection::capture_credential_sample() {
  std::copy(packet_key_.begin(), packet_key_.end(), credential_sample_.begin());
  credential_sample_size_ = packet_key_.size();
  credential_ssl_pending_ = SSL_pending(stream_.native_handle());
  if (credential_ssl_pending_ <= 0) {
    return;
  }

  const auto bytes_to_peek = std::min<int>(credential_ssl_pending_, kNativeCredentialTailBytes);
  const auto bytes_peeked = SSL_peek(
      stream_.native_handle(), credential_sample_.data() + credential_sample_size_, bytes_to_peek);
  if (bytes_peeked > 0) {
    credential_sample_size_ += static_cast<std::size_t>(bytes_peeked);
  }
}

void WorldConnection::read_native_credential_tail() {
  beast::get_lowest_layer(stream_).expires_after(kCredentialTailTimeout);
  asio::async_read(
      stream_, asio::buffer(credential_tail_),
      [self = shared_from_this()](boost::system::error_code error, std::size_t bytes_received) {
        if (error) {
          spdlog::warn("World credential tail read connection={} ended after {} "
                       "of {} bytes: {}",
                       self->connection_id(), bytes_received, kNativeCredentialTailBytes,
                       error.message());
          self->clear_native_credential_tail();
          self->fail("world credential tail read", error);
          return;
        }
        self->on_native_credential_tail();
      });
}

std::span<const char, WorldConnection::kInitialCredentialBytes>
WorldConnection::credential_prefix() const noexcept {
  return packet_key_;
}

std::span<const char> WorldConnection::credential_sample() const noexcept {
  return std::span(credential_sample_).first(credential_sample_size_);
}

int WorldConnection::credential_pending_bytes() const noexcept {
  return credential_ssl_pending_;
}

std::span<const char, WorldConnection::kNativeCredentialTailBytes>
WorldConnection::credential_tail() const noexcept {
  return credential_tail_;
}

void WorldConnection::clear_initial_credential() noexcept {
  std::fill(packet_key_.begin(), packet_key_.end(), 0);
  std::fill(credential_sample_.begin(), credential_sample_.end(), 0);
}

void WorldConnection::clear_native_credential_tail() noexcept {
  std::fill(credential_tail_.begin(), credential_tail_.end(), 0);
}

void WorldConnection::complete_request(std::vector<Frame> frames, bool steady_state) {
  if (closed()) {
    return;
  }
  if (frames.empty()) {
    read_length();
    return;
  }

  const auto enqueued_before = read_gate_.enqueued();
  for (auto &frame : frames) {
    enqueue_frame(std::move(frame));
  }
  park_read(
      ReadResumeGate::choose_park(closed(), steady_state, read_gate_.enqueued() - enqueued_before));
}

void WorldConnection::read_length() {
  if (closed() || read_in_progress_) {
    return;
  }

  read_in_progress_ = true;
  beast::get_lowest_layer(stream_).expires_after(kPacketTimeout);
  spdlog::debug("Native world awaiting client frame connection={}", connection_id());
  asio::async_read(
      stream_, asio::buffer(length_bytes_),
      [self = shared_from_this()](boost::system::error_code error, std::size_t bytes_received) {
        if (error) {
          spdlog::warn("Native world frame length read connection={} ended after "
                       "{} of {} bytes: {}",
                       self->connection_id(), bytes_received, self->length_bytes_.size(),
                       error.message());
          self->fail("world frame length", error);
          return;
        }

        const auto frame_length = static_cast<unsigned>(self->length_bytes_[0]) |
                                  (static_cast<unsigned>(self->length_bytes_[1]) << 8);
        if (frame_length < kClientHeaderBytes) {
          spdlog::warn("Native world frame rejected: invalid client header length");
          self->close_on_io();
          return;
        }

        spdlog::debug("Native world client frame connection={} declared_bytes={}",
                      self->connection_id(), frame_length);
        self->incoming_.bytes.resize(frame_length);
        std::copy(self->length_bytes_.begin(), self->length_bytes_.end(),
                  self->incoming_.bytes.begin());
        self->read_payload();
      });
}

void WorldConnection::read_payload() {
  auto &bytes = incoming_.bytes;
  asio::async_read(
      stream_,
      asio::buffer(bytes.data() + length_bytes_.size(), bytes.size() - length_bytes_.size()),
      [self = shared_from_this()](boost::system::error_code error, std::size_t) {
        self->read_in_progress_ = false;
        if (error) {
          self->fail("world frame payload", error);
          return;
        }

        self->on_frame(std::move(self->incoming_));
      });
}

FrameEnqueueStatus WorldConnection::enqueue_frame(Frame frame) {
  if (closed()) {
    return FrameEnqueueStatus::closed;
  }

  if (frame.local_plan || frame.local_message) {
    spdlog::error("Unbound local frame plan connection={}", connection_id());
    close_on_io();
    return FrameEnqueueStatus::failed;
  }

  // Move the callback out before invoking it: the queued frame must not retain its
  // preparation owner, and the callback may replace the frame's encoded bytes.
  auto prepare = std::move(frame.prepare_for_enqueue);
  if (prepare) {
    try {
      auto workspace = PreparationWorkspace::try_acquire();
      if (!workspace) {
        throw std::runtime_error("World packet preparation workspace is already in use");
      }
      if (!prepare(frame)) {
        return FrameEnqueueStatus::stale;
      }
    } catch (const std::exception &error) {
      frame.after_enqueue = {};
      frame.enqueue_scope.reset();
      spdlog::error("World frame preparation failed connection={}: {}", connection_id(),
                    error.what());
      close_on_io();
      return FrameEnqueueStatus::failed;
    } catch (...) {
      frame.after_enqueue = {};
      frame.enqueue_scope.reset();
      spdlog::error("World frame preparation failed connection={}: unknown exception",
                    connection_id());
      close_on_io();
      return FrameEnqueueStatus::failed;
    }
  }

  if (frame.prepared_batch) {
    auto batch = std::move(frame.prepared_batch);
    auto ordering = std::move(frame.enqueue_scope);
    auto after_enqueue = std::move(frame.after_enqueue);
    std::size_t total_bytes = 0;
    bool valid = batch->size() <= options_.maximum_queued_frames - outbound_frames_.size();
    for (const auto &item : *batch) {
      const auto &payload = item.shared ? *item.shared : item.bytes;
      if (item.prepare_for_enqueue || item.prepared_batch || item.local_plan ||
          item.local_message || payload.empty() ||
          payload.size() > options_.maximum_queued_bytes - queued_bytes_ - total_bytes) {
        valid = false;
        break;
      }
      total_bytes += payload.size();
    }
    if (!valid) {
      after_enqueue = {};
      ordering.reset();
      spdlog::warn("Native world connection={} exceeded catch-up queue limit", connection_id());
      close_on_io();
      return FrameEnqueueStatus::failed;
    }
    try {
      append_frame_batch_atomically(outbound_frames_, *batch, [&] {
        if (after_enqueue) {
          after_enqueue();
        }
      });
    } catch (...) {
      after_enqueue = {};
      ordering.reset();
      spdlog::error("Native world connection={} could not queue catch-up", connection_id());
      close_on_io();
      return FrameEnqueueStatus::failed;
    }
    queued_bytes_ += total_bytes;
    for (std::size_t index = 0; index < batch->size(); ++index) {
      read_gate_.on_enqueued();
    }
    after_enqueue = {};
    ordering.reset();
    if (!batch->empty() && !write_in_progress_) {
      write_next_frame();
    }
    return FrameEnqueueStatus::inserted;
  }

  const auto &payload = frame.shared ? *frame.shared : frame.bytes;
  if (outbound_frames_.size() >= options_.maximum_queued_frames ||
      payload.size() > options_.maximum_queued_bytes - queued_bytes_) {
    spdlog::warn("Native world connection={} exceeded outbound queue limit", connection_id());
    world_metrics::add(world_metrics::CounterId::QueueLimitCloses);
    frame.after_enqueue = {};
    frame.enqueue_scope.reset();
    close_on_io();
    return FrameEnqueueStatus::failed;
  }

  const auto payload_size = payload.size();
  auto ordering = std::move(frame.enqueue_scope);
  auto after_enqueue = std::move(frame.after_enqueue);
  frame.local_allocation.reset();
  try {
    outbound_frames_.push_back(std::move(frame));
    queued_bytes_ += payload_size;
    read_gate_.on_enqueued();
    if (after_enqueue) {
      after_enqueue();
    }
  } catch (const std::exception &error) {
    after_enqueue = {};
    ordering.reset();
    spdlog::error("World frame enqueue failed connection={}: {}", connection_id(), error.what());
    close_on_io();
    return FrameEnqueueStatus::failed;
  } catch (...) {
    after_enqueue = {};
    ordering.reset();
    spdlog::error("World frame enqueue failed connection={}: unknown error", connection_id());
    close_on_io();
    return FrameEnqueueStatus::failed;
  }
  // Completion callbacks must never run under the source publication ordering domain.
  after_enqueue = {};
  ordering.reset();
  if (world_metrics::enabled()) {
    world_metrics::record(world_metrics::HistogramId::OutboundQueueFrames, outbound_frames_.size());
    world_metrics::record(world_metrics::HistogramId::OutboundQueueBytes, queued_bytes_);
  }
  if (!write_in_progress_) {
    write_next_frame();
  }
  return FrameEnqueueStatus::inserted;
}

void WorldConnection::park_read(ReadResumeGate::Park park) {
  switch (park) {
  case ReadResumeGate::Park::drain:
    read_gate_.park_drain();
    break;
  case ReadResumeGate::Park::barrier:
    read_gate_.park_barrier();
    break;
  case ReadResumeGate::Park::none:
    return;
  }
  read_parked_stamp_ = world_metrics::post_stamp();
}

void WorldConnection::maybe_resume_read() {
  if (closed()) {
    return;
  }
  switch (read_gate_.try_resume_by_barrier(outbound_frames_.size(), queued_bytes_)) {
  case ReadResumeGate::Resume::by_barrier:
    world_metrics::add(world_metrics::CounterId::ReadResumesByBarrier);
    world_metrics::record_read_resume_gap(std::exchange(read_parked_stamp_, 0));
    read_length();
    break;
  case ReadResumeGate::Resume::blocked_by_backlog:
    world_metrics::add(world_metrics::CounterId::ReadResumeBlockedByBacklog);
    break;
  default:
    break;
  }
}

void WorldConnection::write_next_frame() {
  if (closed() || write_in_progress_) {
    return;
  }
  if (outbound_frames_.empty()) {
    if (read_gate_.try_resume_by_drain() == ReadResumeGate::Resume::by_drain) {
      world_metrics::add(world_metrics::CounterId::ReadResumesByDrain);
      world_metrics::record_read_resume_gap(std::exchange(read_parked_stamp_, 0));
      read_length();
    }
    return;
  }

  outgoing_ = std::move(outbound_frames_.front());
  outbound_frames_.pop_front();
  const auto &wire = outgoing_.shared ? *outgoing_.shared : outgoing_.bytes;
  queued_bytes_ -= wire.size();
  if (world_metrics::enabled()) {
    world_metrics::count_frame_out(outgoing_.opcode(), wire.size());
  }
  write_in_progress_ = true;
  beast::get_lowest_layer(stream_).expires_after(kPacketTimeout);
  on_frame_started(outgoing_.opcode());
  spdlog::debug("Native world response write connection={} opcode={} bytes={}", connection_id(),
                outgoing_.opcode(), wire.size());
  asio::async_write(
      stream_, asio::buffer(wire),
      [self = shared_from_this()](boost::system::error_code error, std::size_t bytes_written) {
        if (error) {
          self->fail("world frame write", error);
          return;
        }

        spdlog::debug("Native world response written connection={} bytes={}", self->connection_id(),
                      bytes_written);
        self->write_in_progress_ = false;
        self->read_gate_.on_written();
        self->on_frame_written(self->outgoing_.opcode());
        if (self->closed()) {
          return;
        }
        self->maybe_resume_read();
        self->write_next_frame();
      });
}

void WorldConnection::fail(const char *operation, boost::system::error_code error) {
  if (error != asio::error::eof && error != asio::error::operation_aborted &&
      error != ssl::error::stream_truncated) {
    spdlog::warn("Native {} failed: {}", operation, error.message());
  }
  close_on_io();
}

void WorldConnection::close_on_io() {
  if (closed()) {
    return;
  }
  closed_ = true;
  try {
    handshake_delay_timer_.cancel();
  } catch (...) {
    try {
      spdlog::error("World handshake timer cancellation failed connection={}", connection_id());
    } catch (...) {
    }
  }
  boost::system::error_code ignored;
  beast::get_lowest_layer(stream_).socket().close(ignored);
  try {
    on_connection_closed();
  } catch (const std::exception &error) {
    try {
      spdlog::critical("World close cleanup failed connection={}: {}; recovery remains fenced",
                       connection_id(), error.what());
    } catch (...) {
    }
  } catch (...) {
    try {
      spdlog::critical("World close cleanup failed connection={}; recovery remains fenced",
                       connection_id());
    } catch (...) {
    }
  }
}

} // namespace chronicles
