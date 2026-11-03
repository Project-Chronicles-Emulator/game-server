#pragma once

#include "net/pending_frame_budget.hpp"
#include "net/read_resume_gate.hpp"
#include "net/world_tls_diagnostics.hpp"
#include "protocol/protocol.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/ssl.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace chronicles {

enum class FrameEnqueueStatus { inserted, stale, closed, failed, cancelled };

struct WorldConnectionOptions {
  std::uint64_t connection_id{};
  std::chrono::milliseconds handshake_delay{};
  bool credential_diagnostics{};
  std::size_t maximum_queued_frames{};
  std::size_t maximum_queued_bytes{};
  // Optional test/embedding limits for handlers waiting to reach the I/O executor.
  // Zero chooses the server's bounded defaults.
  std::size_t maximum_posted_frames{};
  std::size_t maximum_posted_bytes{};
  std::shared_ptr<GlobalPendingFrameBudget> shared_posted_budget;
};

// One shared object owns the transport and the derived application session. I/O callbacks
// retain that same object; the application may retain it on a separate worker strand.
class WorldConnection : public std::enable_shared_from_this<WorldConnection> {
public:
  virtual ~WorldConnection() = default;

  // These two entrypoints may be called from other threads. Mutable transport state is
  // accessed only by their posted callbacks on the existing single I/O executor.
  void post_frame(Frame frame, std::function<void(FrameEnqueueStatus)> completion = {});
  void request_close();

  bool closed() const noexcept;
  std::uint64_t connection_id() const noexcept;
  const std::string &remote_address() const noexcept;

protected:
  static constexpr std::size_t kInitialCredentialBytes = 32;
  static constexpr std::size_t kNativeCredentialTailBytes = 68;
  static constexpr std::size_t kMaximumCredentialSampleBytes = 100;

  WorldConnection(boost::asio::ip::tcp::socket socket,
                  std::shared_ptr<boost::asio::ssl::context> tls_context,
                  WorldConnectionOptions options);

  // start is called once, after a shared owner exists. Queue/read/close operations run on
  // the single I/O executor. The caller validates its public delay/queue policy.
  void start();
  boost::asio::any_io_executor io_executor();
  void complete_request(std::vector<Frame> frames, bool steady_state);
  FrameEnqueueStatus enqueue_frame(Frame frame);
  std::shared_ptr<PendingFrameBudget::Lease> reserve_posted_frames(std::span<const Frame> frames);
  void close_on_io();
  void read_native_credential_tail();

  // A credential hook may post work retaining this object. No read reuses these buffers
  // until that work posts its acceptance/tail-read/close continuation back to I/O. Only
  // the worker that owns this credential phase may read and clear them off the I/O thread.
  std::span<const char, kInitialCredentialBytes> credential_prefix() const noexcept;
  std::span<const char> credential_sample() const noexcept;
  int credential_pending_bytes() const noexcept;
  std::span<const char, kNativeCredentialTailBytes> credential_tail() const noexcept;
  void clear_initial_credential() noexcept;
  void clear_native_credential_tail() noexcept;

  // Hooks run synchronously on I/O. A written hook runs before read resumption. The
  // close hook sees closed() set and runs before physical socket/timer cancellation.
  virtual void on_transport_ready();
  virtual void on_initial_credential() = 0;
  virtual void on_native_credential_tail() = 0;
  virtual void on_frame(Frame frame) = 0;
  virtual void on_frame_started(std::uint16_t opcode);
  virtual void on_frame_written(std::uint16_t opcode) = 0;
  virtual void on_connection_closed() = 0;

private:
  void disable_nagle_delay();
  void begin_tls_handshake();
  void read_packet_key();
  void capture_credential_sample();
  void read_length();
  void read_payload();
  void park_read(ReadResumeGate::Park park);
  void maybe_resume_read();
  void write_next_frame();
  void fail(const char *operation, boost::system::error_code error);

  const WorldConnectionOptions options_;
  PendingFrameBudget posted_frame_budget_;
  std::atomic<bool> posted_frame_overflow_{};
  std::shared_ptr<boost::asio::ssl::context> tls_context_;
  // OpenSSL callbacks reference this through stream destruction; order is intentional.
  WorldTlsDiagnostics tls_diagnostics_;
  boost::beast::ssl_stream<boost::beast::tcp_stream> stream_;
  boost::asio::steady_timer handshake_delay_timer_;
  std::string remote_address_;
  std::array<char, kInitialCredentialBytes> packet_key_{};
  std::array<char, kNativeCredentialTailBytes> credential_tail_{};
  std::array<char, kMaximumCredentialSampleBytes> credential_sample_{};
  std::size_t credential_sample_size_{};
  int credential_ssl_pending_{};
  std::array<std::uint8_t, 2> length_bytes_{};
  Frame incoming_;
  Frame outgoing_;
  std::deque<Frame> outbound_frames_;
  std::size_t queued_bytes_{};
  bool read_in_progress_{};
  bool write_in_progress_{};
  ReadResumeGate read_gate_;
  std::uint64_t read_parked_stamp_{};
  std::atomic<bool> closed_{};
};

} // namespace chronicles
