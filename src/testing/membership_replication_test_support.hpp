#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace chronicles::testing_support {

using Json = nlohmann::json;

template <typename Context> class ReplicationContext final : public Context {
public:
  void queue_packet(std::uint64_t connection_id, std::uint16_t opcode,
                    const Json &message) override {
    calls.push_back("packet " + std::to_string(connection_id) + ":" + std::to_string(opcode));
    messages.push_back(message);
    if (on_packet) {
      on_packet(connection_id, opcode);
    }
  }

  std::vector<std::uint64_t> observers(std::uint64_t subject_connection_id) override {
    calls.push_back("observers " + std::to_string(subject_connection_id));
    if (fail_observers) {
      throw std::runtime_error("observer view failed");
    }
    return visible[subject_connection_id];
  }

  void queue_shared_observers(std::uint16_t opcode, const Json &message,
                              const std::vector<std::uint64_t> &observers) override {
    calls.push_back("shared " + std::to_string(opcode));
    shared.push_back({message, observers});
  }

  std::vector<std::string> calls;
  std::vector<Json> messages;
  std::vector<std::pair<Json, std::vector<std::uint64_t>>> shared;
  std::map<std::uint64_t, std::vector<std::uint64_t>> visible;
  std::function<void(std::uint64_t, std::uint16_t)> on_packet;
  bool fail_observers{};
};

} // namespace chronicles::testing_support
