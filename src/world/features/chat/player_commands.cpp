#include "world/features/chat/player_commands.hpp"

#include "world/features/chat/native_chat.hpp"

#include <sstream>
#include <stdexcept>
#include <utility>

namespace chronicles {
namespace {

constexpr unsigned kMaximumCommandsPerWindow = 10;
constexpr std::size_t kMaximumCommandSenders = 128;
constexpr auto kCommandRateWindow = std::chrono::seconds(10);

PlayerCommandResult reply(std::vector<std::string> messages) {
  return {true, std::move(messages)};
}

} // namespace

PlayerCommands::PlayerCommands(PvpCommand pvp, std::function<Clock::time_point()> now,
                               MountCommand mount, UnstuckCommand unstuck)
    : pvp_(std::move(pvp)), now_(std::move(now)), mount_(std::move(mount)),
      unstuck_(std::move(unstuck)) {
  if (!now_) {
    throw std::invalid_argument("Player commands require a clock");
  }
}

PlayerCommandResult PlayerCommands::execute(const ChatIdentity &sender, const std::string &text) {
  if (text.empty() || (text.front() != '/' && text.front() != '!')) {
    return {};
  }

  if (sender.world_id <= 0 || sender.character_id <= 0) {
    throw Unauthorized();
  }
  validate_chat_text(text);

  {
    std::lock_guard lock(mutex_);
    const auto now = now_();
    std::erase_if(windows_, [&](const auto &entry) {
      return now - entry.second.started >= kCommandRateWindow;
    });
    const auto key = std::make_pair(sender.world_id, sender.character_id);
    if (!windows_.contains(key) && windows_.size() >= kMaximumCommandSenders) {
      throw std::length_error("Player command rate tracking limit reached");
    }
    auto [window, inserted] = windows_.try_emplace(key, RateWindow{now});
    if (window->second.count >= kMaximumCommandsPerWindow) {
      throw std::length_error("Player command rate exceeded");
    }
    ++window->second.count;
  }

  // The stock client consumes unknown slash commands locally. Bang commands use
  // ordinary chat transport; forwarded slash aliases share this same parser.
  std::istringstream input(text.substr(1));
  std::string command;
  std::string argument;
  std::string extra;
  input >> command >> argument >> extra;

  if (command == "help") {
    if (!argument.empty()) {
      return reply({"Usage: !help"});
    }
    return reply({"=== Server commands ===", "!help - Show available commands.",
                  "!pvp on - Enable your PvP flag.", "!pvp off - Disable your PvP flag.",
                  "!pvp status - Show your current PvP flag.", "!mount - Summon and ride a horse.",
                  "!dismount - Get off your horse or wagon.",
                  "!unstuck - Move to a nearby safe location."});
  }

  if (command == "pvp") {
    if (!extra.empty() ||
        (!argument.empty() && argument != "on" && argument != "off" && argument != "status")) {
      return reply({"Usage: !pvp on | off | status"});
    }
    if (!pvp_) {
      return reply({"PvP commands are unavailable on this server."});
    }
    std::optional<bool> enabled;
    if (argument == "on" || argument == "off") {
      enabled = argument == "on";
    }
    auto result = pvp_(sender, enabled);
    validate_chat_text(result);
    return reply({std::move(result)});
  }

  if (command == "mount") {
    return execute_mount(sender, PlayerMountAction::mount, argument);
  }

  if (command == "dismount") {
    return execute_mount(sender, PlayerMountAction::dismount, argument);
  }

  if (command == "unstuck") {
    if (!argument.empty()) {
      return reply({"Usage: !unstuck"});
    }
    if (!unstuck_) {
      return reply({"Unstuck is unavailable on this server."});
    }
    auto result = unstuck_(sender);
    if (result.empty()) {
      return reply({});
    }
    validate_chat_text(result);
    return reply({std::move(result)});
  }

  return reply({"Unknown command. Type !help for available commands."});
}

PlayerCommandResult PlayerCommands::execute_mount(const ChatIdentity &sender,
                                                  PlayerMountAction action,
                                                  const std::string &argument) {
  if (!argument.empty()) {
    return reply({action == PlayerMountAction::mount ? "Usage: !mount" : "Usage: !dismount"});
  }
  if (!mount_) {
    return reply({"Mount commands are unavailable on this server."});
  }

  auto result = mount_(sender, action);
  if (result.empty()) {
    // Queued: the world session sends the outcome line itself.
    return reply({});
  }
  validate_chat_text(result);
  return reply({std::move(result)});
}

} // namespace chronicles
