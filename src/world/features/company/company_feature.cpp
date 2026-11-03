#include "world/features/company/company_feature.hpp"

#include <utility>

namespace chronicles::company {

CompanyFeature::CompanyFeature(std::shared_ptr<CompanyService> service,
                               std::shared_ptr<const CompanyDialogueIndex> dialogues)
    : service_(std::move(service)), dialogues_(std::move(dialogues)) {
}

std::unique_ptr<CompanySession> CompanyFeature::create_session(CompanySessionContext &context) {
  return std::make_unique<CompanySession>(*this, context);
}

const std::shared_ptr<CompanyService> &CompanyFeature::service() const noexcept {
  return service_;
}

bool CompanyFeature::has_dialogues() const noexcept {
  return static_cast<bool>(dialogues_);
}

CompanySession::CompanySession(CompanyFeature &feature, CompanySessionContext &context)
    : feature_(feature), context_(context) {
}

void CompanySession::on_select_character(const WorldProgress &progress) {
  last_experience_ = progress.experience;
}

void CompanySession::collect_activity_rewards(std::vector<GameplayPacket> &packets) {
  if (!context_.company_tutorial_progress() || !context_.company_selected_character_id() ||
      !last_experience_) {
    return;
  }

  const auto current = context_.company_tutorial_progress()->experience;
  const auto gained = current - *last_experience_;
  last_experience_ = current;
  if (gained <= 0) {
    return;
  }

  const auto character_id = *context_.company_selected_character_id();
  context_.company_publish_activity_experience(character_id, gained);
  if (!feature_.service_) {
    return;
  }
  const auto gain = feature_.service_->record_character_experience(character_id, gained);
  if (!gain) {
    return;
  }
  packets.push_back(company_packet(company_world::kCompanyUpdateNotification, gain->status.record));
  if (gain->position_changed) {
    context_.company_publish_membership_changed();
  }
}

std::vector<GameplayPacket> CompanySession::dialogue(const WorldGameplayRequest &request) {
  const auto sequence = request.sequence;
  if (!feature_.service_ || !feature_.dialogues_ || !context_.company_selected_character_id() ||
      !context_.company_character()) {
    return {company_error_packet(sequence, x5::ec::Dialouge_InvalidSelectionData)};
  }

  const auto &message = request.message;
  const auto action = feature_.dialogues_->resolve(
      message.value("dialogue_cms_id", 0), message.value("sequence_order", 0),
      message.value("select_order", 0), context_.company_dialogue_npc());
  if (action.error != x5::ec::Success) {
    return {company_error_packet(sequence, action.error)};
  }

  const auto character_id = *context_.company_selected_character_id();
  switch (action.task) {
  case CompanyDialogueTask::join: {
    const auto result = feature_.service_->join(character_id, action.company_type);
    if (result.error != x5::ec::Success) {
      return {company_error_packet(sequence, result.error)};
    }
    context_.company_publish_membership_changed();
    return {company_packet(company_world::kJoinCompanyResponse, result.status.record)};
  }
  case CompanyDialogueTask::change: {
    if (context_.company_has_guild_membership(character_id)) {
      return {company_error_packet(sequence, x5::ec::Company_CannotLeave)};
    }
    const auto result = feature_.service_->change(character_id, action.company_type);
    if (result.error != x5::ec::Success) {
      return {company_error_packet(sequence, result.error)};
    }
    return {company_packet(company_world::kJoinCompanyResponse, result.status.record)};
  }
  case CompanyDialogueTask::permit_register:
    return pioneer_permit(sequence, action.company_type, false);
  case CompanyDialogueTask::permit_extend:
    return pioneer_permit(sequence, action.company_type, true);
  }
  return {company_error_packet(sequence, x5::ec::Dialouge_InvalidSelectionData)};
}

std::vector<GameplayPacket> CompanySession::pioneer_permit(std::uint32_t sequence, int company_type,
                                                           bool extend) {
  const auto character_id = *context_.company_selected_character_id();
  const auto status = feature_.service_->status(character_id);
  if (status.record.company_type == kCompanyNone) {
    return {company_error_packet(sequence, x5::ec::Company_IsNotJoined)};
  }
  // A selection that names a company must match the character's; none means its own company.
  if (company_type != kCompanyNone && status.record.company_type != company_type) {
    return {company_error_packet(sequence, x5::ec::Company_NotMatched)};
  }

  const auto plan = feature_.service_->plan_permit(character_id, extend);
  if (plan.error != x5::ec::Success) {
    return {company_error_packet(sequence, plan.error)};
  }

  const auto current = context_.company_current_progress();
  if (!current) {
    return {company_error_packet(sequence, x5::ec::Dialouge_InvalidSelectionData)};
  }

  const auto &before = *current;
  const auto charge = charge_permit(before, plan);
  if (charge.error != x5::ec::Success) {
    return {company_error_packet(sequence, charge.error)};
  }
  const auto committed = feature_.service_->commit_paid_permit(
      context_.company_character()->account_id, plan, before, charge.after);
  if (committed.error != x5::ec::Success) {
    return {company_error_packet(sequence, committed.error)};
  }
  context_.company_publish_progress_committed(charge.after);

  std::vector<GameplayPacket> packets;
  if (charge.items_update) {
    packets.push_back(*charge.items_update);
  }
  packets.push_back(
      company_packet(company_world::kRegisterPioneerPermitResponse, committed.status.record));
  return packets;
}

} // namespace chronicles::company
