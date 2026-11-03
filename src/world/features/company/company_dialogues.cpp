#include "world/features/company/company_dialogues.hpp"

#include "data/client_catalog.hpp"
#include "cms.generated.pb.h"

#include <spdlog/spdlog.h>

namespace chronicles {
namespace {

std::optional<CompanyDialogueTask> task_from_case(const std::string &task_case) {
  if (task_case == "CompanyJoin") {
    return CompanyDialogueTask::join;
  }
  if (task_case == "CompanyChange") {
    return CompanyDialogueTask::change;
  }
  if (task_case == "PioneerPermitRegist") {
    return CompanyDialogueTask::permit_register;
  }
  if (task_case == "PioneerPermitExtend") {
    return CompanyDialogueTask::permit_extend;
  }
  return std::nullopt;
}

std::optional<int> company_from_enum_name(const std::string &name) {
  x5::cms::EX5CompanyType type{};
  if (!x5::cms::EX5CompanyType_Parse(name, &type) || type == x5::cms::EX5CompanyType_None) {
    return std::nullopt;
  }
  return static_cast<int>(type);
}

// DialogueCondition.type holds a JSON object body such as
// "companyType":"EX5CompanyType_BlueSalt","companyDialogueConditionType":"...".
std::optional<int> condition_company(const nlohmann::json &row) {
  if (catalog_text_field(row, "typeCase") != "SystemCompany") {
    return std::nullopt;
  }
  const auto body =
      nlohmann::json::parse("{" + catalog_text_field(row, "type") + "}", nullptr, false);
  if (!body.is_object() || !body.contains("companyType") || !body["companyType"].is_string()) {
    return std::nullopt;
  }
  return company_from_enum_name(body["companyType"].get<std::string>());
}

} // namespace

std::shared_ptr<const CompanyDialogueIndex> CompanyDialogueIndex::load(ClientCatalog &catalog) {
  for (const auto *table : {"DialogueSelection", "Dialogue", "DialogueCondition", "Npc"}) {
    if (!catalog.has_table(table)) {
      spdlog::warn("Company dialogues: catalog has no {} table; company dialogues disabled",
                   table);
      return from_rows({}, {}, {}, {});
    }
  }
  return from_rows(catalog.rows("DialogueSelection"), catalog.rows("Dialogue"),
                   catalog.rows("DialogueCondition"), catalog.rows("Npc"));
}

std::shared_ptr<const CompanyDialogueIndex>
CompanyDialogueIndex::from_rows(const std::vector<nlohmann::json> &selections,
                                const std::vector<nlohmann::json> &dialogues,
                                const std::vector<nlohmann::json> &conditions,
                                const std::vector<nlohmann::json> &npcs) {
  auto index = std::shared_ptr<CompanyDialogueIndex>(new CompanyDialogueIndex());

  std::map<int, int> companies_by_condition;
  for (const auto &row : conditions) {
    const auto condition_id = catalog_integer(row, "conditionId");
    const auto company = condition_company(row);
    if (condition_id && company) {
      companies_by_condition.emplace(static_cast<int>(*condition_id), *company);
    }
  }

  for (const auto &row : selections) {
    const auto task = task_from_case(catalog_text_field(row, "playerTaskCase"));
    const auto id = catalog_integer(row, "id");
    const auto sequence = catalog_integer(row, "sequenceOrder");
    const auto select = catalog_integer(row, "selectOrder");
    if (!task || !id || !sequence || !select) {
      continue;
    }

    Selection selection;
    selection.task = *task;
    const auto condition_id = catalog_integer(row, "dialogueConditionId");
    if (condition_id) {
      const auto found = companies_by_condition.find(static_cast<int>(*condition_id));
      if (found != companies_by_condition.end()) {
        selection.condition_company = found->second;
      }
    }
    const auto key = std::tuple{static_cast<int>(*id), static_cast<int>(*sequence),
                                static_cast<int>(*select)};
    index->selections_.emplace(key, selection);
    index->speakers_[static_cast<int>(*id)];
  }

  for (const auto &row : dialogues) {
    const auto id = catalog_integer(row, "id");
    const auto speaker = catalog_integer(row, "speakerNameValue");
    if (!id || !speaker || catalog_text_field(row, "speakerName") != "Npc") {
      continue;
    }
    const auto dialogue = index->speakers_.find(static_cast<int>(*id));
    if (dialogue != index->speakers_.end()) {
      dialogue->second.insert(static_cast<int>(*speaker));
    }
  }

  std::set<int> speaker_ids;
  for (const auto &[dialogue_id, speakers] : index->speakers_) {
    speaker_ids.insert(speakers.begin(), speakers.end());
  }
  for (const auto &row : npcs) {
    const auto id = catalog_integer(row, "id");
    if (!id || !speaker_ids.contains(static_cast<int>(*id))) {
      continue;
    }
    const auto company = company_from_enum_name("EX5CompanyType_" +
                                                catalog_text_field(row, "company"));
    if (company) {
      index->npc_companies_.emplace(static_cast<int>(*id), *company);
    }
  }

  return index;
}

CompanyDialogueAction CompanyDialogueIndex::resolve(int dialogue_id, int sequence_order,
                                                    int select_order,
                                                    std::optional<int> dialogue_npc_cms_id) const {
  CompanyDialogueAction action;
  const auto selection = selections_.find({dialogue_id, sequence_order, select_order});
  if (selection == selections_.end()) {
    action.error = x5::ec::Dialouge_InvalidSelectionData;
    return action;
  }
  if (!dialogue_npc_cms_id) {
    action.error = x5::ec::Dialogue_InvalidTarget;
    return action;
  }

  // A dialogue spoken by known NPCs may only be used while talking to one of them.
  const auto speakers = speakers_.find(dialogue_id);
  if (speakers != speakers_.end() && !speakers->second.empty() &&
      !speakers->second.contains(*dialogue_npc_cms_id)) {
    action.error = x5::ec::Dialogue_InvalidTarget;
    return action;
  }

  action.task = selection->second.task;
  action.company_type = selection->second.condition_company;
  if (action.company_type == 0) {
    const auto npc_company = npc_companies_.find(*dialogue_npc_cms_id);
    if (npc_company != npc_companies_.end()) {
      action.company_type = npc_company->second;
    }
  }

  // Joining or changing needs a target company. A permit is always issued by the character's
  // own company, so a permit desk without one (Devicas, dialogue 10500) stays valid.
  const bool needs_company =
      action.task == CompanyDialogueTask::join || action.task == CompanyDialogueTask::change;
  if (needs_company && action.company_type == 0) {
    action.error = x5::ec::Company_InvalidCmsId;
  }
  return action;
}

std::size_t CompanyDialogueIndex::selection_count() const {
  return selections_.size();
}

} // namespace chronicles
