#include "world/features/party/party_replication.hpp"

namespace chronicles::party {

void PartyFeature::replicate_update(const PartyUpdate &update,
                                    PartyReplicationContext &context) const {
  for (const auto &packet : update.packets) {
    context.queue_packet(packet.connection_id, packet.opcode, packet.message);
  }

  // Membership changes reach the subject first, then the clients currently seeing it.
  // Private tutorial subjects have no shared-presence observers.
  for (const auto &change : update.unit_changes) {
    const auto message = PartyWorld::change_party_unit_message(change);
    context.queue_packet(change.subject_connection_id, kChangePartyUnitNotification, message);
    const auto observers = context.observers(change.subject_connection_id);
    if (!observers.empty()) {
      context.queue_shared_observers(kChangePartyUnitNotification, message, observers);
    }
  }
}

} // namespace chronicles::party
