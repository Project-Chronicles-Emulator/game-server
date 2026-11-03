#pragma once

// Company dialogue functions: which PC2S_DialogueFunctionReq selections join or change a
// company or register/extend a pioneer permit, and which company they act on.
//
// Evidence: DialogueSelection.playerTaskCase CompanyJoin, CompanyChange, PioneerPermitRegist
// and PioneerPermitExtend; the client sends DialogueFunctionReq {dialogue_cms_id,
// sequence_order, select_order} for these cases only after its own check passes
// (AAC_Dialogue_ExecuteSelectionPlayerTask_Candidate, PlayerTask cases 4, 5, 17 and 18).
// The company comes from the selection's SystemCompany dialogue condition when it has one,
// otherwise from the dialogue NPC's Npc.company. The NPC must be a speaker of that dialogue.

#include "ec.generated.pb.h"

#include <nlohmann/json.hpp>

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace chronicles {

class ClientCatalog;

enum class CompanyDialogueTask { join, change, permit_register, permit_extend };

struct CompanyDialogueAction {
  x5::ec::EErrorCode error{x5::ec::Success};
  CompanyDialogueTask task{CompanyDialogueTask::join};
  // The company the selection names. Zero for a permit task whose selection and NPC name no
  // company (Devicas serves every company): the permit is then for the character's own company.
  int company_type{};
};

class CompanyDialogueIndex {
public:
  static std::shared_ptr<const CompanyDialogueIndex> load(ClientCatalog &catalog);

  // Builds the index from exported rows (DialogueSelection, Dialogue, DialogueCondition and
  // Npc). Unrelated rows are ignored.
  static std::shared_ptr<const CompanyDialogueIndex>
  from_rows(const std::vector<nlohmann::json> &selections,
            const std::vector<nlohmann::json> &dialogues,
            const std::vector<nlohmann::json> &conditions,
            const std::vector<nlohmann::json> &npcs);

  // `dialogue_npc_cms_id` is the NPC the character is talking to, from the session's
  // DialogueStartReq; nullopt when no dialogue is open.
  CompanyDialogueAction resolve(int dialogue_id, int sequence_order, int select_order,
                                std::optional<int> dialogue_npc_cms_id) const;

  std::size_t selection_count() const;

private:
  struct Selection {
    CompanyDialogueTask task{CompanyDialogueTask::join};
    // From a SystemCompany dialogue condition; zero when the selection has none.
    int condition_company{};
  };

  std::map<std::tuple<int, int, int>, Selection> selections_;
  // Speaker NPC CMS IDs of each dialogue that has a company selection.
  std::map<int, std::set<int>> speakers_;
  // Npc.company of every speaker that belongs to a company.
  std::map<int, int> npc_companies_;
};

} // namespace chronicles
