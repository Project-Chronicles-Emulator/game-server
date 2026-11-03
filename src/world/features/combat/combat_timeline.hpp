#pragma once

#include <cstddef>
#include <compare>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>

namespace chronicles::combat {

// Orders registered work within one shared world or instance copy. The owner must register
// ingress before dispatch and publish every scheduled successor when completing its predecessor.
// A late event older than the claimed frontier is rejected; callers must fail closed rather
// than silently reordering it. This class does not itself register events or wake owners.
class CombatTimeline final {
public:
  struct Owner {
    std::uint64_t id{};
    std::uint64_t generation{};

    auto operator<=>(const Owner &) const = default;
  };

  enum class Kind { input, milestone, close };

  struct Token {
    std::int64_t tick{};
    std::uint64_t sequence{};
    Owner owner;

    bool operator==(const Token &) const = default;
  };

  enum class EnqueueStatus { accepted, full, stale_tick, invalid, sequence_exhausted };

  struct EnqueueResult {
    EnqueueStatus status{EnqueueStatus::invalid};
    std::optional<Token> token;
  };

  enum class ClaimStatus { ready, waiting, not_due, stale };

  struct ClaimResult {
    ClaimStatus status{ClaimStatus::stale};
    std::optional<Owner> head_owner;
  };

  struct Successor {
    std::int64_t tick{};
    Kind kind{};
  };

  explicit CombatTimeline(std::size_t capacity, bool require_registered_owner = false);

  // Strict populations allocate generations themselves, so a retired owner's token cannot be
  // reused by a later session with the same id. Registration must be supervised externally.
  std::optional<Owner> register_owner(std::uint64_t owner_id);
  bool seal_owner(Owner owner);
  bool retire_owner_if_idle(Owner owner);

  EnqueueResult enqueue(Owner owner, std::int64_t tick, Kind kind);
  ClaimResult try_claim(const Token &token, std::int64_t now_tick);
  bool is_claimed(const Token &token) const;
  // A milestone starts at most once; its token cannot complete until the owner has retained
  // and settled the resulting packets and durable effects, then marks that outcome staged.
  bool begin_milestone(const Token &token);
  bool mark_milestone_staged(const Token &token);
  // A stale deadline may be replaced only before execution begins; earlier replacements are
  // rejected because the claimed frontier cannot be rolled back.
  EnqueueResult replace_unstarted_milestone(
      const Token &token, std::optional<Successor> successor = std::nullopt);
  // If successor insertion throws or is rejected, the head remains claimed. The owner must
  // retry completion without reapplying the already-executed milestone.
  EnqueueResult complete(const Token &token, std::optional<Successor> successor = std::nullopt);
  // Settle a claimed input and atomically replace that owner's queued milestone. The new
  // milestone is inserted before either existing entry is removed, so insertion failure
  // leaves the input claimed and the old deadline intact. Equal deadlines keep their
  // original registration order.
  EnqueueResult complete_input_with_milestone(
      const Token &input, std::optional<Token> previous_milestone,
      std::optional<std::int64_t> next_tick);
  void set_before_insert_hook_for_test(std::function<void()> hook);
  void set_after_insert_hook_for_test(std::function<void()> hook);
  // Unstarted input may be dropped on transport close. A combat milestone needs an ordered
  // claimed close event as proof that cancellation precedes its deadline.
  bool cancel_queued(const Token &token);
  bool cancel_milestone_after_close(const Token &close, const Token &milestone);
  bool owner_has_work(Owner owner) const;
  std::optional<Owner> head_owner() const;
  std::size_t size() const;

private:
  struct Key {
    std::int64_t tick{};
    std::uint64_t sequence{};

    auto operator<=>(const Key &) const = default;
  };

  struct Entry {
    Owner owner;
    Kind kind{};
    bool executing{};
    bool staged{};
  };

  struct OwnerRegistration {
    Owner owner;
    bool sealed{};
    bool close_submitted{};
  };

  std::optional<Token> insert_locked(Owner owner, std::int64_t tick, Kind kind,
                                     std::optional<Token> replaced_milestone = std::nullopt);

  mutable std::mutex mutex_;
  const std::size_t capacity_;
  const bool require_registered_owner_;
  std::map<Key, Entry> entries_;
  // One live milestone per owner generation, including its currently claimed milestone.
  std::map<Owner, Token> owner_milestones_;
  std::map<std::uint64_t, OwnerRegistration> registered_owners_;
  std::uint64_t next_owner_generation_{1};
  std::optional<Token> claimed_;
  std::optional<std::int64_t> frontier_tick_;
  std::uint64_t next_sequence_{1};
  std::function<void()> before_insert_hook_for_test_;
  std::function<void()> after_insert_hook_for_test_;
};

} // namespace chronicles::combat
