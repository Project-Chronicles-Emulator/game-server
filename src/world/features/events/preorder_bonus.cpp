#include "world/features/events/preorder_bonus.hpp"

#include <stdexcept>

namespace chronicles::preorder_bonus {

GameplayPacket respond_without_entitlement(const WorldGameplayRequest &request,
                                           bool authenticated) {
  if (request.opcode != kStateRequest && request.opcode != kClaimRequest) {
    throw std::invalid_argument("Expected a preorder bonus state or claim request");
  }

  const bool claiming = request.opcode == kClaimRequest;
  const char *error = "Success";
  if (!authenticated) {
    error = "InvalidState";
  } else if (claiming) {
    error = "PreorderBonus_NotEligible";
  }

  // PacketSeq is an application correlation value, independent of transport sequence.
  // Missing means the protobuf default, as in the official request/response pairs.
  const auto sequence = request.message.value("packet_seq", nlohmann::json::object());
  return {claiming ? kClaimResponse : kStateResponse,
          {{"ec", error}, {"state", "PreorderBonusState_NotEligible"}, {"packet_seq", sequence}}};
}

} // namespace chronicles::preorder_bonus
