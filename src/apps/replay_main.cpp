#include "protocol/protocol.hpp"
#include "apps/replay_transport.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <thread>

namespace {

constexpr std::uint32_t kMaximumCaptureGapMilliseconds = 60000;
constexpr std::size_t kFrameFragmentSize = 3;

struct ReplayOptions {
  bool paced{};
  bool strict_tail{};
  bool tcp_loopback{};
};

struct ReplayStatistics {
  std::map<unsigned, std::size_t> opcodes;
  std::map<std::string, std::size_t> statuses;
  std::size_t replayed{};
  std::size_t frame_round_trips{};
};

ReplayOptions parse_options(int argc, char **argv, std::ofstream &output) {
  ReplayOptions options;

  for (int i = 3; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--paced") {
      options.paced = true;
    } else if (argument == "--tcp-loopback") {
      options.tcp_loopback = true;
    } else if (argument == "--strict-tail") {
      options.strict_tail = true;
    } else if (argument == "--jsonl" && i + 1 < argc) {
      if (std::filesystem::exists(argv[i + 1]) &&
          std::filesystem::equivalent(argv[i + 1], argv[1])) {
        throw std::runtime_error("JSONL output must not overwrite the input capture");
      }

      output.open(argv[++i], std::ios::binary);
      if (!output) {
        throw std::runtime_error("Cannot open output");
      }
    } else {
      throw std::runtime_error("Unknown/incomplete argument: " + argument);
    }
  }

  return options;
}

void verify_frame_round_trip(chronicles::Protocol &protocol, const chronicles::Frame &frame,
                             const nlohmann::json &result) {
  std::optional<std::uint32_t> sequence;
  if (result.contains("sequence")) {
    sequence = result.at("sequence").get<std::uint32_t>();
  }

  const auto encoded = protocol.encode(frame.opcode(), result.at("message"), sequence);
  const auto decoded = protocol.decode(encoded);
  if (decoded.at("status") != "decoded" || decoded.at("message") != result.at("message")) {
    throw std::runtime_error("Application frame encode/decode changed protobuf semantics");
  }
}

void decode_record(chronicles::Protocol &protocol, const chronicles::Record &record,
                   ReplayStatistics &statistics, std::ofstream &output) {
  auto result = protocol.decode(record.frame);
  if (result.at("status") == "decoded") {
    verify_frame_round_trip(protocol, record.frame, result);
    ++statistics.frame_round_trips;
  }

  result["log_offset"] = record.offset;
  result["elapsed_ms"] = record.elapsed_ms;
  ++statistics.statuses[result.at("status").get<std::string>()];
  ++statistics.opcodes[record.frame.opcode()];
  ++statistics.replayed;

  if (output.is_open()) {
    output << result.dump() << '\n';
    if (!output) {
      throw std::runtime_error("JSONL output write failed");
    }
  }
}

void replay_offline(const std::vector<chronicles::Record> &records, bool paced,
                    const std::function<void(const chronicles::Record &)> &consume) {
  std::uint32_t previous_elapsed_ms = 0;
  bool first_record = true;

  for (const auto &record : records) {
    if (paced && !first_record) {
      const auto gap_ms = static_cast<std::uint32_t>(record.elapsed_ms - previous_elapsed_ms);
      if (gap_ms > kMaximumCaptureGapMilliseconds) {
        throw std::runtime_error("Capture gap exceeds 60s; replay without --paced to skip waits");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(gap_ms));
    }
    previous_elapsed_ms = record.elapsed_ms;
    first_record = false;

    // Exercise the same assembler as live application frames with a deliberate split.
    chronicles::FrameStream stream;
    auto consume_frame = [&](chronicles::Frame frame) {
      consume({record.offset, record.elapsed_ms, std::move(frame)});
    };
    const auto split = std::min(kFrameFragmentSize, record.frame.bytes.size());
    stream.feed(std::span(record.frame.bytes).first(split), consume_frame);
    stream.feed(std::span(record.frame.bytes).subspan(split), consume_frame);
    if (stream.pending()) {
      throw std::runtime_error("Replay left incomplete application frame");
    }
  }
}

} // namespace

int main(int argc, char **argv) {
  try {
    if (argc < 3) {
      std::cerr << "Usage: chronicles-replay LOG PROTOCOL_DIR [--jsonl FILE] [--tcp-loopback] "
                   "[--paced] [--strict-tail]\n";
      return 1;
    }

    std::ofstream output;
    const auto options = parse_options(argc, argv, output);
    chronicles::Protocol protocol(argv[2]);
    const auto bytes = chronicles::read_file(argv[1]);
    ReplayStatistics statistics;

    std::vector<chronicles::Record> records;
    const auto log_info = chronicles::read_log(
        bytes, [&](const chronicles::Record &record) { records.push_back(record); });
    auto consume_record = [&](const chronicles::Record &record) {
      decode_record(protocol, record, statistics, output);
    };

    if (options.tcp_loopback) {
      chronicles::replay_tcp(records, options.paced, consume_record);
    } else {
      replay_offline(records, options.paced, consume_record);
    }

    if (output.is_open()) {
      output.flush();
      if (!output) {
        throw std::runtime_error("JSONL output flush failed");
      }
    }

    const nlohmann::json summary{{"build", log_info.build},
                                 {"records", log_info.records},
                                 {"replayed", statistics.replayed},
                                 {"semantic_frame_roundtrips", statistics.frame_round_trips},
                                 {"observed_opcodes", statistics.opcodes.size()},
                                 {"opcode_mappings", protocol.opcode_count()},
                                 {"statuses", statistics.statuses},
                                 {"incomplete_tail_bytes", log_info.tail_bytes},
                                 {"complete_end", log_info.complete_end},
                                 {"mode", options.tcp_loopback ? "tcp-loopback" : "offline"},
                                 {"paced", options.paced}};
    std::cout << summary.dump(2) << '\n';

    if (statistics.statuses["decoded"] != statistics.replayed ||
        (options.strict_tail && log_info.tail_bytes)) {
      return 2;
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "Replay failed: " << error.what() << '\n';
    return 1;
  }
}
