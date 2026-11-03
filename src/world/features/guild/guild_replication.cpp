#include "world/features/guild/guild_replication.hpp"

namespace chronicles::guild {

void GuildFeature::replicate_update(const guild_world::GuildWorldUpdate &update,
                                    GuildReplicationContext &context) const {
  if (!world_) {
    return;
  }

  for (const auto &packet : update.packets) {
    const auto connection = world_->connection_of(packet.char_id);
    if (connection) {
      context.queue_packet(*connection, packet.opcode, packet.message);
    }
  }

  for (const auto &change : update.unit_changes) {
    const auto subject = world_->connection_of(change.char_id);
    if (!subject) {
      continue;
    }

    const auto message = GuildWorld::change_guild_unit_message(change);
    context.queue_packet(*subject, guild_world::kChangeGuildUnitNotification, message);
    const auto observers = context.observers(*subject);
    if (!observers.empty()) {
      context.queue_shared_observers(guild_world::kChangeGuildUnitNotification, message, observers);
    }
  }
}

} // namespace chronicles::guild
