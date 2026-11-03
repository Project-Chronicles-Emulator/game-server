#pragma once

#include "persistence/character_state_repository.hpp"
#include "persistence/quest_effect_settlement_status.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace chronicles {

struct QuestEffectReconciliation {
  QuestEffectSettlementStatus status{QuestEffectSettlementStatus::retry};
  std::int64_t character_id{};
  std::string effect_id;
  bool event_completed{};
};

struct QuestEffectComponent {
  // Reconciles an in-memory uncertain operation even if it no longer appears in the outbox.
  std::function<std::optional<QuestEffectReconciliation>()> reconcile_uncertain;
  std::function<QuestEffectSettlementStatus(const QuestServiceEffectEvent &)> settle;
  // Called after authoritative state is installed. Delivery is a best-effort resync; it is
  // not a durable or exactly-once network acknowledgement.
  std::function<void(std::int64_t)> resynchronize;
};

struct QuestEffectDispatchResult {
  std::size_t completed{};
  bool retry_needed{};
};

// All callers share one dispatcher, so startup and runtime recovery cannot race each other.
// Storage guards still enforce per-character and per-guild FIFO against external callers.
class QuestEffectDispatcher {
public:
  QuestEffectDispatcher(std::shared_ptr<CharacterStateRepository> repository,
                        QuestEffectComponent company, QuestEffectComponent guild);

  QuestEffectDispatchResult dispatch(int world_id, std::optional<std::int64_t> character_id,
                                     std::size_t limit);

  // Before admitting players, drain every oldest-per-character page within a hard event cap.
  // An unavailable component, unresolved error, or excess backlog aborts startup.
  std::size_t recover_startup(int world_id, std::size_t maximum_events);

private:
  bool reconcile(const QuestEffectComponent &component,
                 std::vector<QuestEffectReconciliation> &completed);
  bool reconcile_reservations();
  bool settle(const QuestEffectComponent &component, const QuestServiceEffectEvent &event);
  bool resynchronize(const QuestEffectComponent &component, std::int64_t character_id) const;

  struct PendingResynchronization {
    std::int64_t character_id{};
    std::string effect_id;
    bool ready{};
    bool company_needed{};
    bool guild_needed{};
  };
  PendingResynchronization &reserve_resynchronization(std::int64_t character_id,
                                                       const std::string &effect_id);
  bool flush_resynchronizations();

  std::shared_ptr<CharacterStateRepository> repository_;
  QuestEffectComponent company_;
  QuestEffectComponent guild_;
  std::vector<PendingResynchronization> pending_resynchronizations_;
  std::uint64_t next_after_sequence_{};
  std::mutex mutex_;
};

} // namespace chronicles
