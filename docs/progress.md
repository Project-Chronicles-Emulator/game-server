# Feature progress

[← Project home](../README.md) · [Contribution workflow](guides/feature-workflow.md)

This matrix reads its labels directly from `protocol/feature-coverage.json`.
It does not estimate a completion percentage or promote packet coverage into gameplay.
Open a feature to read its supported behavior, remaining limits, sources and evidence.

## How to read the labels

- **Implemented within documented scope:** the bounded emulator contract is implemented;
  official parity, all content and original-client acceptance may still be limited.
- **Partial:** meaningful behavior exists, but the feature has explicit remaining work.
- **Missing:** no complete behavior-producing service is established for the feature.

**31 feature groups:** 3 bounded implementations, 27 partial and 1 missing.
These are feature-group labels, not player-readiness or test-coverage percentages.

## Full feature matrix

| Feature | Current label | Implementation and limits |
| :--- | :--- | :--- |
| **Accounts, authentication and realm discovery** | 🟡 Partial | [Read the feature](protocol-feature-reference.md#accounts-authentication-and-realm-discovery) |
| **Character lobby and identity** | 🟡 Partial | [Read the feature](protocol-feature-reference.md#character-lobby-and-identity) |
| **World connection, clock and session lifecycle** | 🟡 Partial | [Read the feature](protocol-feature-reference.md#world-connection-clock-and-session-lifecycle) |
| **Movement, map travel, gliding and teleportation** | 🟡 Partial | [Read the feature](protocol-feature-reference.md#movement-map-travel-gliding-and-teleportation) |
| **World population, visibility and NPC AI** | 🟡 Partial | [Read the feature](protocol-feature-reference.md#world-population-visibility-and-npc-ai) |
| **Combat, skill stages and damage** | 🟡 Partial | [Read the feature](protocol-feature-reference.md#combat-skill-stages-and-damage) |
| **Projectiles and skill objects** | 🟡 Partial | [Read the feature](protocol-feature-reference.md#projectiles-and-skill-objects) |
| **Status effects, recovery, auras and abnormal gauges** | 🟡 Partial | [Read the feature](protocol-feature-reference.md#status-effects-recovery-auras-and-abnormal-gauges) |
| **Level, EXP, stats, perks and achievements** | 🟡 Partial | [Read the feature](protocol-feature-reference.md#level-exp-stats-perks-and-achievements) |
| **Quest chains, objectives and rewards** | 🟡 Partial | [Read the feature](protocol-feature-reference.md#quest-chains-objectives-and-rewards) |
| **Dialogue, adventure objects, doors and cutscenes** | 🟡 Partial | [Read the feature](protocol-feature-reference.md#dialogue-adventure-objects-doors-and-cutscenes) |
| **Inventory, equipment, currency and utility bag** | 🟡 Partial | [Read the feature](protocol-feature-reference.md#inventory-equipment-currency-and-utility-bag) |
| **Corpse loot and reward ownership** | 🟡 Partial | [Read the feature](protocol-feature-reference.md#corpse-loot-and-reward-ownership) |
| **NPC shops, sales and repairs** | 🟡 Partial | [Read the feature](protocol-feature-reference.md#npc-shops-sales-and-repairs) |
| **Gathering, cooking, crafting and item improvement** | 🟡 Partial | [Read the feature](protocol-feature-reference.md#gathering-cooking-crafting-and-item-improvement) |
| **World/local/group chat and whispers** | 🟡 Partial | [Read the feature](protocol-feature-reference.md#worldlocalgroup-chat-and-whispers) |
| **Parties, leadership and group channels** | 🟡 Partial | [Read the feature](protocol-feature-reference.md#parties-leadership-and-group-channels) |
| **Friends, lookup and presence** | 🟡 Partial | [Read the feature](protocol-feature-reference.md#friends-lookup-and-presence) |
| **Guilds, companies and membership** | 🟡 Partial | [Read the feature](protocol-feature-reference.md#guilds-companies-and-membership) |
| **Mounts, mates and summoned NPCs** | 🟡 Partial | [Read the feature](protocol-feature-reference.md#mounts-mates-and-summoned-npcs) |
| **Housing, land contracts and placed objects** | 🟡 Partial | [Read the feature](protocol-feature-reference.md#housing-land-contracts-and-placed-objects) |
| **Trading posts, wagons, vouchers and trade runs** | 🟢 Implemented within documented scope | [Read the feature](protocol-feature-reference.md#trading-posts-wagons-vouchers-and-trade-runs) |
| **Player marketplace (auction listings)** | 🟢 Implemented within documented scope | [Read the feature](protocol-feature-reference.md#player-marketplace-auction-listings) |
| **Mail, storage boxes and delivery history** | 🟡 Partial | [Read the feature](protocol-feature-reference.md#mail-storage-boxes-and-delivery-history) |
| **Rankings, raids and channel events** | 🟡 Partial | [Read the feature](protocol-feature-reference.md#rankings-raids-and-channel-events) |
| **Paid shop, Steam purchases and preorder rewards** | ⚪ Missing | [Read the feature](protocol-feature-reference.md#paid-shop-steam-purchases-and-preorder-rewards) |
| **Diagnostics, GM commands and anti-cheat messaging** | 🟡 Partial | [Read the feature](protocol-feature-reference.md#diagnostics-gm-commands-and-anti-cheat-messaging) |
| **Conversational AI agent-engine REST services** | 🟡 Partial | [Read the feature](protocol-feature-reference.md#conversational-ai-agent-engine-rest-services) |
| **Client preferences, exploration REST and feedback** | 🟡 Partial | [Read the feature](protocol-feature-reference.md#client-preferences-exploration-rest-and-feedback) |
| **Emotes, social animations and selfie mode** | 🟢 Implemented within documented scope | [Read the feature](protocol-feature-reference.md#emotes-social-animations-and-selfie-mode) |
| **Common messages and service announcements** | 🟡 Partial | [Read the feature](protocol-feature-reference.md#common-messages-and-service-announcements) |

## Practical focus areas

| Area | Supported foundation | Important remaining work |
| :--- | :--- | :--- |
| Login | Account sessions, launch grants, separate login/game admission | General platform authentication; client acceptance after the split |
| Movement | Checkpoints, nearby movement, bounded tutorial travel | General portals/channels, navigation and broader world authority |
| Combat | Server timing, costs/cooldowns, supported actions and effects | Full defence/effect behavior, broader combat and client acceptance |
| Quests | Supported objectives, revisioned state and atomic rewards | Complete content and objective semantics; broader client playthroughs |
| Gathering / crafting | Validated gathering sources, claims and supported processing | Expertise/level benefits and remaining authored policies/content |
| Housing | Catalog-backed contracts, placed contents, permissions/storage | Complete ordinary-client contract/edit/placement/lifecycle acceptance |
| Trade / marketplace | Bounded stateful trade and marketplace contracts | Official economic values and original-client workflow acceptance |
| Social | Supported chat, friends, party and guild operations | Remaining audiences, travel, company behavior and persistence limits |

These short descriptions are a reading aid. The linked canonical feature section
takes precedence and contains details that a one-line summary cannot represent.

## Evidence is separate from status

Static recovery establishes a client contract; a capture establishes observed traffic;
a synthetic check establishes the tested service/storage/transport behavior; an
original-client trial establishes only the path and build actually observed.
The latest split has synthetic two-process PostgreSQL/TLS verification, not a new
original-client gameplay acceptance. See [validation](service-split-validation.md).

## Updating this page

Review the owning feature and evidence, update canonical coverage and regenerate:

```powershell
python tools/build_project_progress.py
python tools/build_project_progress.py --check
```

This summary is generated. Update its generator when its reading guidance changes;
update canonical coverage when the supported contract changes. Follow the full
[reference maintenance procedure](protocol-reference-maintenance.md) for feature work.
