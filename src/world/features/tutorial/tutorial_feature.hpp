#pragma once

#include "world/features/tutorial/tutorial.hpp"

#include <memory>

namespace chronicles::tutorial {

// The diagnostic tutorial profile has a recovered shared start for these four classes.
bool supports_diagnostic_tutorial_start(int class_id);

// The loaded immutable profile belongs to this server feature; each player owns its instance.
class TutorialFeature {
public:
  explicit TutorialFeature(std::shared_ptr<const TutorialData> data);

  const std::shared_ptr<const TutorialData> &data() const noexcept;

private:
  const std::shared_ptr<const TutorialData> data_;
};

} // namespace chronicles::tutorial
