#pragma once

#include "persistence/character_state_types.hpp"
#include "world/features/mail/mail_types.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace chronicles {

struct MemoryDatabase;

// Account and character mail: delivery, listing, batch changes and the composite claim.
// Implementations: MemoryMailRepository and PostgresMailRepository.
class MailRepository {
public:
  virtual ~MailRepository() = default;

  // Mail is addressed to an account and optionally one character in a world. Delivery
  // keys are unique per account/world and survive deletion. Listing excludes tombstones
  // and expired messages; account mail is visible to every owned character until claimed.
  virtual std::optional<MailRecord> deliver_mail(MailRecord mail) = 0;

  virtual std::vector<MailRecord> mailbox(std::int64_t account_id,
                                         std::int64_t character_id, int world_id,
                                         std::int64_t now_epoch_ticks) = 0;

  // A batch is all-or-nothing. Unclaimed, unexpired attachments cannot be deleted.
  virtual bool change_mail(std::int64_t account_id, std::int64_t character_id, int world_id,
                            const std::vector<std::int64_t> &mail_ids, bool remove,
                            std::int64_t now_epoch_ticks) = 0;

  // On claimed, progress, inventory and the durable claim receipt commit together. A retry
  // returns already_claimed without applying its proposed transition. The expected mail
  // snapshot guards attachment replacement; the character revision guards gameplay races.
  virtual MailClaimStatus claim_mail(std::int64_t account_id, std::int64_t character_id,
                                     int world_id, const MailRecord &expected_mail,
                                     std::int64_t now_epoch_ticks,
                                     const WorldProgress &before, const WorldProgress &after,
                                     std::vector<CharacterItem> &granted_items) = 0;
};

// Memory implementation of MailRepository. The repository holds the shared MemoryDatabase
// by shared_ptr and owns the mail table and id sequence; every access locks
// database_->mutex like the former in-memory member. The constructor registers a
// CharacterDeletionHook that erases a deleted character's recipient mail, and the
// destructor neutralizes it so the callback never outlives the table.
class MemoryMailRepository final : public MailRepository {
public:
  explicit MemoryMailRepository(std::shared_ptr<MemoryDatabase> database);
  ~MemoryMailRepository() override;

  // The registered hook callback points at this repository's table, so copying is forbidden.
  MemoryMailRepository(const MemoryMailRepository &) = delete;
  MemoryMailRepository &operator=(const MemoryMailRepository &) = delete;

  std::optional<MailRecord> deliver_mail(MailRecord mail) override;

  std::vector<MailRecord> mailbox(std::int64_t account_id, std::int64_t character_id,
                                  int world_id, std::int64_t now_epoch_ticks) override;

  bool change_mail(std::int64_t account_id, std::int64_t character_id, int world_id,
                            const std::vector<std::int64_t> &mail_ids, bool remove,
                            std::int64_t now_epoch_ticks) override;

  MailClaimStatus claim_mail(std::int64_t account_id, std::int64_t character_id,
                                     int world_id, const MailRecord &expected_mail,
                                     std::int64_t now_epoch_ticks,
                                     const WorldProgress &before, const WorldProgress &after,
                                     std::vector<CharacterItem> &granted_items) override;

private:
  std::shared_ptr<MemoryDatabase> database_;
  // Position of this repository's entry in database_->deletion_hooks. The destructor
  // neutralizes it so the callback never outlives the mail table.
  std::size_t deletion_hook_index_{};
  std::int64_t next_mail_id_{1};
  std::map<std::int64_t, MailRecord> mails_;
};

// PostgreSQL implementation of MailRepository. The repository keeps its own copy of the
// connection string and opens one pqxx::connection per call; the character row lock
// serializes each claim commit.
class PostgresMailRepository final : public MailRepository {
public:
  explicit PostgresMailRepository(std::string connection) : connection_(std::move(connection)) {}

  std::optional<MailRecord> deliver_mail(MailRecord mail) override;

  std::vector<MailRecord> mailbox(std::int64_t account_id, std::int64_t character_id, int world_id,
                                  std::int64_t now_epoch_ticks) override;

  bool change_mail(std::int64_t account_id, std::int64_t character_id, int world_id,
                            const std::vector<std::int64_t> &mail_ids, bool remove,
                            std::int64_t now_epoch_ticks) override;

  MailClaimStatus claim_mail(std::int64_t account_id, std::int64_t character_id,
                                     int world_id, const MailRecord &expected_mail,
                                     std::int64_t now_epoch_ticks,
                                     const WorldProgress &before, const WorldProgress &after,
                                     std::vector<CharacterItem> &granted_items) override;

private:
  std::string connection_;
};

} // namespace chronicles
