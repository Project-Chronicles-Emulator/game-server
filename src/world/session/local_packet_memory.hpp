#pragma once

#include "protocol/gameplay_packet.hpp"

#include <cstddef>
#include <stdexcept>

namespace chronicles {

// A local plan retains a JSON copy until the recipient's I/O executor runs. Charge each
// container, key and allocation conservatively instead of counting only protobuf wire bytes.
inline constexpr std::size_t kMaximumLocalPacketRetainedBytes = 16 * 1024 * 1024;
inline constexpr std::size_t kJsonContainerAllocationOverhead = 128;
inline constexpr std::size_t kMaximumLocalPacketDepth = 64;

inline void charge_local_packet_bytes(std::size_t amount, std::size_t &total) {
  if (total > kMaximumLocalPacketRetainedBytes ||
      amount > kMaximumLocalPacketRetainedBytes - total) {
    throw std::runtime_error("Local packet metadata exceeds the retained-byte limit");
  }
  total += amount;
}

inline void charge_local_json(const nlohmann::json &value, std::size_t depth, std::size_t &total) {
  if (depth > kMaximumLocalPacketDepth) {
    throw std::runtime_error("Local packet JSON exceeds the nesting limit");
  }
  charge_local_packet_bytes(sizeof(nlohmann::json), total);
  if (value.is_object()) {
    const auto &object = value.get_ref<const nlohmann::json::object_t &>();
    charge_local_packet_bytes(sizeof(object) + kJsonContainerAllocationOverhead, total);
    for (const auto &[key, child] : object) {
      charge_local_packet_bytes(sizeof(nlohmann::json::object_t::value_type) +
                                    kJsonContainerAllocationOverhead,
                                total);
      if (key.capacity() >= kMaximumLocalPacketRetainedBytes) {
        throw std::runtime_error("Local packet key exceeds the retained-byte limit");
      }
      charge_local_packet_bytes(key.capacity() + 1, total);
      charge_local_json(child, depth + 1, total);
    }
  } else if (value.is_array()) {
    const auto &array = value.get_ref<const nlohmann::json::array_t &>();
    const auto capacity_limit = kMaximumLocalPacketRetainedBytes / sizeof(nlohmann::json);
    if (array.capacity() > capacity_limit) {
      throw std::runtime_error("Local packet array exceeds the retained-byte limit");
    }
    charge_local_packet_bytes(sizeof(array) + kJsonContainerAllocationOverhead, total);
    charge_local_packet_bytes(array.capacity() * sizeof(nlohmann::json), total);
    for (const auto &child : array) {
      charge_local_json(child, depth + 1, total);
    }
  } else if (value.is_string()) {
    const auto &text = value.get_ref<const nlohmann::json::string_t &>();
    charge_local_packet_bytes(sizeof(text) + kJsonContainerAllocationOverhead, total);
    if (text.capacity() >= kMaximumLocalPacketRetainedBytes) {
      throw std::runtime_error("Local packet string exceeds the retained-byte limit");
    }
    charge_local_packet_bytes(text.capacity() + 1, total);
  } else if (value.is_binary()) {
    throw std::runtime_error("Binary JSON is unsupported in local packet metadata");
  }
}

inline std::size_t local_packet_retained_bytes(const GameplayPacket &packet) {
  std::size_t total = kJsonContainerAllocationOverhead;
  charge_local_packet_bytes(packet.local_plan->retained_bytes(), total);
  charge_local_json(packet.message, 0, total);
  return total;
}

} // namespace chronicles
