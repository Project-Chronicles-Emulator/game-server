# Implement a feature or fix a bug

[← Contributing](../../CONTRIBUTING.md) · [Architecture](architecture.md) · [Testing](testing.md)

## 1. Describe the behavior before the code

For a feature, write the complete stateful flow: who can request it, required state,
effects, response/notifications, persistence, retry/reconnect behavior and expiry.
For a bug, capture the smallest failing sequence and the expected result. Establish
whether it fails in the client, transport, adapter, domain rules, storage or presentation.
Do not replace a missing gameplay flow with a generic success response or GM command.

An example contract for gathering is stronger than “handle the gathering packet”:
the actor owns the session, is in the right instance and range, has the required tool,
cannot start while dead/busy, consumes the source under its supported policy, receives
one committed reward, updates quest state atomically and cannot duplicate it on retry.

## 2. Establish what is known

Read the owning section in `docs/protocol-feature-reference.md` and then its source
and evidence. Use `rg` to locate the native request, feature and repository methods.

| Evidence | Establishes | Does not establish |
| :--- | :--- | :--- |
| Recovered descriptor/opcode | Build-specific fields, types and mapping | Runtime behavior or future-build compatibility |
| Static client analysis | A traced branch, sender, parser or UI consumer | That the whole flow works on this emulator |
| Official capture | Observed request/response sequence and values | Every possible condition or official general rule |
| Synthetic check | The contract exercised by that check | Original-client display or a live playthrough |
| Original-client trial | The observed path on the recorded build | Complete system parity or production capacity |

Mark uncertain interpretations **Candidate** and explain why. If a policy is custom,
label it custom rather than presenting a fitted rule as official. A schema/capture
count is not a list of fully implemented game features.

## 3. Find the owner and preserve boundaries

Use the [source map](architecture.md). Follow an existing similar path before adding
a helper or service. Reuse module utilities without forcing unrelated behaviors into
one abstraction. Define cross-repository API changes before implementing each side.

Keep framing/serialization, native adapters, feature rules, persistence and process
orchestration separate. Do not introduce another runtime language or process for
gameplay without a concrete architectural need. Extraction tooling can use its
existing Python/.NET path without changing the C++ runtime stack.

## 4. Design for state, not just the happy packet

For a mutation, decide:

- Which account, character, map/instance, object and revision own it?
- Which ranges, sizes, quantities, timestamps and rate bounds apply?
- Which values come from the catalog, and which require behavioral evidence?
- What commits together? What prevents duplicate effects after a lost reply?
- What happens if the player disconnects, reconnects, changes map or retries?
- Which worker/strand serializes it, and which locks are acquired in what order?
- What happens when the database/private authority is unavailable?

Use existing ownership fences and atomic transactions. Commit durable effects before
sending success. A client hit/effect report cannot invent or accelerate authoritative
damage, resource use or rewards. Keep blocking work off socket executors.

## 5. Make a focused change and reproduce the failure

Keep names descriptive, functions cohesive and asynchronous ownership explicit.
Write a regression check for a meaningful failure: duplicate reward, stale revision,
wrong owner, expiration, replay or ordering. Avoid tests that merely repeat the new
implementation. For a behavior-preserving refactor, retain the original public
contract and check its ordering/state effects rather than weakening assertions.

Do not edit generated `.pb.cc` files. Fix `tools/generate_protos.py` or its versioned
input when generation is wrong. Do not change unknown client fields/IDs to make a
test pass; understand their meaning or keep the uncertainty visible.

## 6. Validate the actual path

Build the affected targets, run relevant checks and exercise the service path where
the bug occurred. Persistence changes need a disposable PostgreSQL fixture as well
as memory checks. Protocol/native ordering may need replay or TLS/native-adapter
checks. Client-visible success needs an original-client trial before claiming that
display/interaction is fixed. Use the [testing guide](testing.md) and report gaps.

Never restart someone else's runtime or change a live database to obtain test data.
Agree on build/runtime ownership in a shared workspace and use isolated fixtures.

## 7. Update the feature record in the same PR

Update `protocol/feature-coverage.json`: supported scope, missing work, packet
overrides, actual verification and source/test pointers. A packet becoming handled
does not automatically make the parent feature implemented. Preserve historical
acceptance dates and build identities.

After inspecting the affected source changes:

```powershell
python tools/build_protocol_reference.py --refresh-reviewed-sources
python tools/build_project_progress.py
```

Follow [reference maintenance](../protocol-reference-maintenance.md) to synchronize
the published reference and record confirmed readback. Then:

```powershell
python tools/build_protocol_reference.py --check
python tools/build_project_progress.py --check
```

Public contributors may not have access to the existing private Pages. Regenerate
locally, include the output and explicitly flag publication as pending for a
maintainer; do not manufacture a publication receipt or hide a failing publication
check. The repository reference remains available to every contributor.

## 8. Make review easy

Describe the trigger and before/after behavior, your evidence, why the change belongs
in that module, what checks passed and what remains unverified. Link related issues
and coordinated PRs. Avoid claiming completion because only one branch, one packet,
an administrative shortcut or an offline fixture passed.
