#pragma once

// Separate copies of instance and dungeon maps.
//
// Map.chMaxCount gives instances and dungeons several channels, and the client's channel
// invite flow (PartyChannelInvite*) lets a party share one. The exact allocation rules of the
// official server are not in the client export. CUSTOM policy: a character in an established
// party enters its party's copy of the map; a character without a party enters a copy of its
// own. Everyone in one copy shares its monsters, gathering nodes and doodad state and sees the
// other players in it; players in other copies of the same map see none of that.
//
// A copy lives while at least one session holds it. Sessions keep their copy across same-map
// handoffs and release it when they leave the map, so the next entry after everyone left builds
// a fresh copy (the official reset of an emptied instance is not recovered).

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <typeindex>
#include <utility>

namespace chronicles {

class InstanceCopy;

// Synthetic presence maps of copies are -(kCopyPresenceMapBase + slot). Housing channels use
// -(zone_key * 1000 + channel), far below this base, and real map ids are positive.
inline constexpr int kCopyPresenceMapBase = 1'000'000'000;
inline constexpr std::size_t kMaximumLiveCopies = 1'000'000;

// Owner key spaces. The solo key equals the one combat used for per-character instances.
inline constexpr std::uint64_t kSoloCopyOwnerBit = std::uint64_t{1} << 63;
inline constexpr std::uint64_t kPartyCopyOwnerBit = std::uint64_t{1} << 62;
// Party, loot and companion channel keys of copies. Tutorial instances use the character id
// and shared maps zero, so this bit keeps copy keys apart from both.
inline constexpr std::uint64_t kCopyChannelKeyBit = std::uint64_t{1} << 62;

class InstanceCopies {
public:
  // `copy_maps` lists the map ids that get separate copies (instances and dungeons).
  explicit InstanceCopies(std::set<int> copy_maps,
                          std::size_t maximum_live_copies = kMaximumLiveCopies);

  bool copies(int map_id) const;

  static std::uint64_t solo_owner(std::int64_t character_id);
  static std::uint64_t party_owner(std::int64_t party_id);

  // The copy a character enters on `map_id`: its party's copy when it belongs to an established
  // party, else its own. Null for maps without copies. Throws std::runtime_error when the live
  // copy bound is reached.
  std::shared_ptr<InstanceCopy> enter(int map_id, std::int64_t character_id,
                                      std::optional<std::int64_t> party_id);

  std::size_t live_copies() const;

private:
  friend class InstanceCopy;

  struct Entry {
    std::weak_ptr<InstanceCopy> copy;
    std::uint64_t serial{};
  };

  // Outlives the registry while copies exist, so a copy released late still frees its slot.
  struct Shared {
    std::mutex mutex;
    std::map<std::pair<int, std::uint64_t>, Entry> copies;
    std::set<int> free_slots;
    int next_slot{};
    std::uint64_t next_serial{1};
  };

  std::set<int> copy_maps_;
  std::size_t maximum_live_copies_;
  std::shared_ptr<Shared> shared_;
};

class InstanceCopy {
public:
  InstanceCopy(const InstanceCopy &) = delete;
  InstanceCopy &operator=(const InstanceCopy &) = delete;
  ~InstanceCopy();

  int map_id() const noexcept;
  std::uint64_t owner() const noexcept;

  // Unique for the life of the process; never reused by a later copy.
  std::uint64_t serial() const noexcept;

  // Negative presence map that keeps this copy's players apart from other copies.
  int presence_map() const noexcept;

  // Channel key for party credit, loot sharing and companion visibility.
  std::uint64_t channel_key() const noexcept;

  // One state object of type `Resource` per copy (its monsters, its gathering nodes, ...),
  // created by `make` on first use. The copy owns it, so it lives exactly as long as the copy.
  template <typename Resource, typename Factory>
  std::shared_ptr<Resource> resource(Factory &&make) {
    std::lock_guard lock(mutex_);
    // The pointer type also names resources whose class is incomplete at the call site.
    auto &slot = resources_[std::type_index(typeid(Resource *))];
    if (!slot) {
      std::shared_ptr<Resource> created = make();
      slot = created;
      return created;
    }
    return std::static_pointer_cast<Resource>(slot);
  }

private:
  friend class InstanceCopies;

  InstanceCopy(int map_id, std::uint64_t owner, std::uint64_t serial, int slot,
               std::weak_ptr<InstanceCopies::Shared> registry);

  const int map_id_;
  const std::uint64_t owner_;
  const std::uint64_t serial_;
  const int slot_;
  std::weak_ptr<InstanceCopies::Shared> registry_;
  std::mutex mutex_;
  std::map<std::type_index, std::shared_ptr<void>> resources_;
};

} // namespace chronicles
