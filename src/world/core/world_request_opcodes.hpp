#pragma once

#include <cstdint>

// World-transport PC2S request IDs that more than one module needs. The frame pipeline routes
// by these IDs and the owning features decode the payloads, so both must read the same
// definition: a typo in a private copy would silently turn a request into an unhandled one.
// Each feature still owns its payload and its other IDs. Front-WebSocket IDs are a separate
// namespace and are not listed here.
namespace chronicles::world_request {

inline constexpr std::uint16_t kNetworkDelayRequest = 20004;

// PC2S_GmCmdReq and PC2S_GmCmdTextReq: structured and console GM commands.
inline constexpr std::uint16_t kGmCommandRequest = 20008;
inline constexpr std::uint16_t kGmCommandTextRequest = 20009;

// Skills and combat (combat.cpp, tutorial_player_session.cpp, nuia_session.cpp).
inline constexpr std::uint16_t kStartSkillRequest = 20010;
inline constexpr std::uint16_t kSkillEffectReportRequest = 20011;
inline constexpr std::uint16_t kExecuteSpawnProjectileRequest = 20012;
inline constexpr std::uint16_t kChangeSkillStageRequest = 20013;
inline constexpr std::uint16_t kClientProjectileHitRequest = 20021;
inline constexpr std::uint16_t kDespawnClientProjectileRequest = 20022;
inline constexpr std::uint16_t kUpdateProjectileRequest = 20023;
inline constexpr std::uint16_t kStartGlidingRequest = 20032;
inline constexpr std::uint16_t kStartRootMotionRequest = 20033;
inline constexpr std::uint16_t kAdaptedRotationRequest = 20034;

// Quest book requests (world_quests.cpp, tutorial.cpp).
inline constexpr std::uint16_t kAcceptQuestRequest = 20016;
inline constexpr std::uint16_t kAbandonQuestRequest = 20017;
inline constexpr std::uint16_t kClaimQuestRewardRequest = 20018;
inline constexpr std::uint16_t kProgressQuestRequest = 20019;
// PC2S_QuestJobBoardReq: open or pay-refresh a quest job board (quest_job_boards.cpp).
inline constexpr std::uint16_t kQuestJobBoardRequest = 20020;

// PC2S_SendChatReq; on the world transport the client sends only Party chat.
inline constexpr std::uint16_t kWorldChatRequest = 20038;

inline constexpr std::uint16_t kReviveRequest = 20039;

// PC2S_EscapeReq (player escape, trading escape and housing departure).
inline constexpr std::uint16_t kEscapeRequest = 20048;

// Adventure objects (Nui tears and statues).
inline constexpr std::uint16_t kActivateAdventureObjectRequest = 20049;
inline constexpr std::uint16_t kDiscoverAdventureObjectRequest = 20050;

// PC2S_InteractUnitReq: merchants, repairable wagons and Nui statues.
inline constexpr std::uint16_t kInteractionUnitRequest = 20068;

// PC2S_StartInteractionAnimReq: gathering, crafting, statues, the housing door toggle and
// housing furniture poses.
inline constexpr std::uint16_t kInteractionStartRequest = 20069;

// PC2S_ExecuteInteractionReq: the execute event of gathering, crafting and garden interactions.
inline constexpr std::uint16_t kInteractionExecuteRequest = 20070;

// PC2S_EndInteractionAnimReq: ends gathering, crafting and housing furniture poses.
inline constexpr std::uint16_t kInteractionEndRequest = 20071;

// PC2S_ItemUseReq: consumables, summon registration items.
inline constexpr std::uint16_t kItemUseRequest = 20077;

inline constexpr std::uint16_t kDiscardRequest = 20072;
inline constexpr std::uint16_t kEquipRequest = 20074;
inline constexpr std::uint16_t kWeaponSlotActivationRequest = 20078;
inline constexpr std::uint16_t kPolishRequest = 20075;

inline constexpr std::uint16_t kMapLoadCompleteRequest = 20085;

// Leaving a housing area: PC2S_ZoneChannelMoveReq (crossing the area's channel volume) and
// PC2S_EscapeReq. Candidate: no capture shows which one the client sends at the boundary.
inline constexpr std::uint16_t kZoneChannelMoveRequest = 20090;

} // namespace chronicles::world_request
