#pragma once

#include "auth/auth.hpp"

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace chronicles {

struct PlayerCommandResult {
  bool handled{};
  std::vector<std::string> replies;
};

// What `!mount` and `!dismount` ask the player's world session to do.
enum class PlayerMountAction {
  mount,
  dismount,
};

class PlayerCommands {
public:
  // nullopt queries status. The world service must authorize a live session and return the
  // actual result. Missing callbacks never report that a PvP flag was changed.
  using PvpCommand = std::function<std::string(const ChatIdentity &, std::optional<bool>)>;

  // Queues a mount or dismount on the player's world session. An empty result means the request
  // was queued and the world session sends the outcome line itself; any other text is the reply
  // (for example "not in the world"). A missing callback never claims an outcome.
  using MountCommand = std::function<std::string(const ChatIdentity &, PlayerMountAction)>;
  using UnstuckCommand = std::function<std::string(const ChatIdentity &)>;

  explicit PlayerCommands(PvpCommand pvp = {}, std::function<Clock::time_point()> now = Clock::now,
                          MountCommand mount = {}, UnstuckCommand unstuck = {});

  // Every slash/bang-prefixed input is consumed privately, including unknown commands.
  // Normal text returns handled=false without consuming a command rate-limit entry.
  PlayerCommandResult execute(const ChatIdentity &sender, const std::string &text);

private:
  struct RateWindow {
    Clock::time_point started;
    unsigned count{};
  };

  PlayerCommandResult execute_mount(const ChatIdentity &sender, PlayerMountAction action,
                                    const std::string &argument);

  PvpCommand pvp_;
  std::function<Clock::time_point()> now_;
  MountCommand mount_;
  UnstuckCommand unstuck_;
  std::mutex mutex_;
  std::map<std::pair<int, std::int64_t>, RateWindow> windows_;
};

} // namespace chronicles
