#include "world/features/tutorial/tutorial_feature.hpp"

#include "cms.generated.pb.h"

#include <utility>

namespace chronicles::tutorial {

bool supports_diagnostic_tutorial_start(int class_id) {
  switch (static_cast<x5::cms::EX5CharacterClass>(class_id)) {
  case x5::cms::EX5CharacterClass_Fight:
  case x5::cms::EX5CharacterClass_Wild:
  case x5::cms::EX5CharacterClass_Adamant:
  case x5::cms::EX5CharacterClass_Magic:
    return true;
  default:
    return false;
  }
}

TutorialFeature::TutorialFeature(std::shared_ptr<const TutorialData> data)
    : data_(std::move(data)) {
}

const std::shared_ptr<const TutorialData> &TutorialFeature::data() const noexcept {
  return data_;
}

} // namespace chronicles::tutorial
