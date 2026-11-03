#include "world/core/instance_copies.hpp"

#include <stdexcept>

namespace chronicles {

InstanceCopies::InstanceCopies(std::set<int> copy_maps, std::size_t maximum_live_copies)
    : copy_maps_(std::move(copy_maps)), maximum_live_copies_(maximum_live_copies),
      shared_(std::make_shared<Shared>()) {
  if (maximum_live_copies_ > kMaximumLiveCopies) {
    throw std::invalid_argument("Instance copy limit exceeds the presence map range");
  }
}

bool InstanceCopies::copies(int map_id) const {
  return copy_maps_.contains(map_id);
}

std::uint64_t InstanceCopies::solo_owner(std::int64_t character_id) {
  return kSoloCopyOwnerBit | static_cast<std::uint64_t>(character_id);
}

std::uint64_t InstanceCopies::party_owner(std::int64_t party_id) {
  return kPartyCopyOwnerBit | static_cast<std::uint64_t>(party_id);
}

std::shared_ptr<InstanceCopy> InstanceCopies::enter(int map_id, std::int64_t character_id,
                                                    std::optional<std::int64_t> party_id) {
  if (!copies(map_id)) {
    return {};
  }

  const auto owner = party_id ? party_owner(*party_id) : solo_owner(character_id);
  const std::pair key{map_id, owner};

  std::lock_guard lock(shared_->mutex);
  const auto existing = shared_->copies.find(key);
  if (existing != shared_->copies.end()) {
    if (auto live = existing->second.copy.lock()) {
      return live;
    }
  }

  if (shared_->free_slots.empty() &&
      static_cast<std::size_t>(shared_->next_slot) >= maximum_live_copies_) {
    throw std::runtime_error("The live instance copy bound is reached");
  }

  const int slot = shared_->free_slots.empty() ? shared_->next_slot : *shared_->free_slots.begin();
  const auto serial = shared_->next_serial;

  // Until registration succeeds, destruction must not acquire the registry lock held here.
  // This also leaves slot and serial accounting unchanged if allocation or insertion throws.
  std::shared_ptr<InstanceCopy> created(
      new InstanceCopy(map_id, owner, serial, slot, std::weak_ptr<Shared>{}));
  shared_->copies.insert_or_assign(key, Entry{created, serial});
  created->registry_ = shared_;
  if (shared_->free_slots.empty()) {
    ++shared_->next_slot;
  } else {
    shared_->free_slots.erase(shared_->free_slots.begin());
  }
  ++shared_->next_serial;
  return created;
}

std::size_t InstanceCopies::live_copies() const {
  std::lock_guard lock(shared_->mutex);
  std::size_t live{};
  for (const auto &[key, entry] : shared_->copies) {
    if (!entry.copy.expired()) {
      ++live;
    }
  }
  return live;
}

InstanceCopy::InstanceCopy(int map_id, std::uint64_t owner, std::uint64_t serial, int slot,
                           std::weak_ptr<InstanceCopies::Shared> registry)
    : map_id_(map_id), owner_(owner), serial_(serial), slot_(slot),
      registry_(std::move(registry)) {
}

InstanceCopy::~InstanceCopy() {
  const auto registry = registry_.lock();
  if (!registry) {
    return;
  }

  std::lock_guard lock(registry->mutex);
  const auto found = registry->copies.find({map_id_, owner_});
  // A later entry may already have replaced this expired copy under the same key.
  if (found != registry->copies.end() && found->second.serial == serial_) {
    registry->copies.erase(found);
  }
  registry->free_slots.insert(slot_);
}

int InstanceCopy::map_id() const noexcept {
  return map_id_;
}

std::uint64_t InstanceCopy::owner() const noexcept {
  return owner_;
}

std::uint64_t InstanceCopy::serial() const noexcept {
  return serial_;
}

int InstanceCopy::presence_map() const noexcept {
  return -(kCopyPresenceMapBase + slot_);
}

std::uint64_t InstanceCopy::channel_key() const noexcept {
  return kCopyChannelKeyBit | serial_;
}

} // namespace chronicles
