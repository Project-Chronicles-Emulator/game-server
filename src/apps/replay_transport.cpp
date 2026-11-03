#include "apps/replay_transport.hpp"

#include <array>
#include <chrono>
#include <exception>
#include <thread>
#include <utility>

#include <boost/asio.hpp>

namespace chronicles {
namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

// A non-aligned read size exercises fragmented and coalesced application frames.
constexpr std::size_t kReceiveBufferSize = 4093;
constexpr std::uint32_t kMaximumCaptureGapMilliseconds = 60000;

void receive_records(tcp::socket &socket, const std::vector<Record> &records,
                     const std::function<void(const Record &)> &consume) {
  FrameStream stream;
  std::array<std::uint8_t, kReceiveBufferSize> buffer{};
  std::size_t record_index = 0;

  for (;;) {
    boost::system::error_code read_error;
    const auto bytes_read = socket.read_some(asio::buffer(buffer), read_error);
    if (bytes_read) {
      stream.feed(std::span(buffer).first(bytes_read), [&](Frame frame) {
        if (record_index >= records.size()) {
          throw std::runtime_error("TCP replay produced extra frame");
        }

        const auto &original = records[record_index++];
        if (frame.bytes != original.frame.bytes) {
          throw std::runtime_error("TCP replay changed application bytes");
        }
        consume(Record{original.offset, original.elapsed_ms, std::move(frame)});
      });
    }

    if (read_error == asio::error::eof) {
      break;
    }
    if (read_error) {
      throw boost::system::system_error(read_error);
    }
  }

  if (stream.pending() || record_index != records.size()) {
    throw std::runtime_error("Incomplete TCP replay");
  }
}

void send_records(tcp::socket &socket, const std::vector<Record> &records, bool paced) {
  std::uint32_t previous_elapsed_ms = 0;
  bool first_record = true;

  for (const auto &record : records) {
    if (paced && !first_record) {
      const auto gap_ms = record.elapsed_ms - previous_elapsed_ms;
      if (gap_ms > kMaximumCaptureGapMilliseconds) {
        throw std::runtime_error("Capture gap exceeds 60s");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(gap_ms));
    }
    previous_elapsed_ms = record.elapsed_ms;
    first_record = false;

    // Scatter header and body over separate writes; TCP may fragment or coalesce freely.
    const auto &bytes = record.frame.bytes;
    asio::write(socket, asio::buffer(bytes.data(), 1));
    asio::write(socket, asio::buffer(bytes.data() + 1, bytes.size() - 1));
  }
}

} // namespace

void replay_tcp(const std::vector<Record> &records, bool paced,
                const std::function<void(const Record &)> &consume) {
  asio::io_context io;
  tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
  tcp::socket client(io);
  client.connect(acceptor.local_endpoint());
  tcp::socket server = acceptor.accept();

  std::exception_ptr receive_error;
  std::exception_ptr send_error;
  std::thread receiver([&] {
    try {
      receive_records(server, records, consume);
    } catch (...) {
      receive_error = std::current_exception();
      boost::system::error_code close_error;
      server.close(close_error);
    }
  });

  try {
    send_records(client, records, paced);
  } catch (...) {
    send_error = std::current_exception();
  }

  // The receiver must finish before inspecting its exception or returning to the caller.
  boost::system::error_code shutdown_error;
  client.shutdown(tcp::socket::shutdown_send, shutdown_error);
  receiver.join();

  if (receive_error) {
    std::rethrow_exception(receive_error);
  }
  if (send_error) {
    std::rethrow_exception(send_error);
  }
}

} // namespace chronicles
