#pragma once

#include "persistence/character_repository.hpp"

#include <cstdint>
#include <optional>

namespace chronicles::character {

// Revisioned progress over one authenticated character. Both live requests and entry grants
// retain their original AuthService save operation; an item trade is a different command.
class CharacterProgressStore {
public:
  virtual ~CharacterProgressStore() = default;

  virtual std::int64_t progression_character_id() const = 0;
  virtual std::optional<Character> progression_character() = 0;
  virtual WorldProgress progression_stored_progress() = 0;
  virtual bool progression_save(const WorldProgress &before, const WorldProgress &after) = 0;
};

} // namespace chronicles::character
