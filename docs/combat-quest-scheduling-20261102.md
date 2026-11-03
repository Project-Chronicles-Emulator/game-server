# Native combat ordering and retained quest events

The 2026-11-02 continuation connects the existing population timeline to native input,
scheduled gameplay and close. Each population or instance copy retains its own ordering;
the existing session worker executes a claim at its logical tick. A skill deadline that has
not yet launched a projectile is included. Successor publication completes the claim before
another owner advances. Wake keys include both population and owner generation.

Map departure remains part of its admitted input. Close cancels later deadlines only after
earlier work has settled. Failed producer execution is not replayed: the existing close
supervisor retains the actual session, settles durable outcomes, and detaches before releasing
the claim. A close whose insertion failed was never admitted; its retry uses the current clock.
An accepted close keeps its original tick. This is process-local ordering, not restart recovery
for transient attacks, projectiles or vitals.

Combat quest events settle at successful receive, advance and direct-item-effect boundaries,
before monster rewards can start a successor quest. The retained batch captures character,
progress, position, scalar quest view, membership and a copied quest book. Preparation retries
use that copy. A prepared transaction keeps its operation identity, source fingerprint, item
rows, mutation and output through a lost COMMIT acknowledgement. A confirmed departed view
does not receive obsolete output. Genuine state conflicts remain fenced; they are not replanned
against newer quest attempts. Uncertain producers and failures before a trustworthy snapshot
retain raw events and admission for recovery rather than awarding or discarding them.

The fresh-install baseline uses schema 44, matching the reward monitor. The unused
schema-45 item-continuation experiment and old offline compaction scripts have been
retired from the distribution. Ordinary
character deletion works without that experimental table and still respects its pending owners
when explicitly installed. Persistent item consumption and subsequent combat/quest effects are
not claimed to be one crash-atomic transaction.

## Verification

- Default Release combat suite: 373 tests, zero failures, 19 generated-profile-dependent skips.
  The reverse-poll regression now exercises the native schedule adapter with an unlaunched
  earlier stun. It checks target health and distinguishes observer damage notifications.
- Quest owner suite: 44 passing tests, including prepared lost acknowledgement, exact retry,
  retained conflict, no-op events and preparation failure with a changed live inventory.
- Schedule/input admission checks: eight passing tests, including retained completion failure
  and two-owner close retry after another owner advances the frontier.
- Generator suite: 334 tests, zero failures, three fixture-dependent skips.
- Disposable PostgreSQL reward monitor and backup/restore rehearsals passed. Restoration
  deliberately excludes post-backup progress; no destructive in-place downgrade is promised.
- Final standard CTest registered 2,752 cases with 261 fixture-dependent skips. Four fixture
  lifecycle failures were repaired and all four passed focused CTest rechecks. Reconnect tests
  now observe admission retirement; two permanent-quarantine tests explicitly assert blocked
  drain and end only their isolated test infrastructure. Ordinary drained teardown is unchanged.
- With the staged combat profile, 373 component cases ran with zero failures and eight skips.
  Fifteen native quest/close fault cases and the shutdown fault case passed. Fourteen separate
  companion fault cases still require their own placement fixtures and were skipped.
- Three native TLS cases passed against the staged profiles: two-client shared monsters,
  dungeon announcements and PvP reconnect state. The legacy drowning fixture also passed
  using identical catalog/tutorial bytes without optional shrine profiles. Its full-profile
  in-place revival assertion still conflicts with shrine revival; the earlier d55 comparison
  reproduces that failure. This is not a claim that full-profile drowning revival passed.
- PostgreSQL combat receipts, experimental schema-45 isolation, startup leases, scheduler
  process recovery and admin quarantine restart/recovery checks passed. Schema 44 remains
  protected from the old schema-40 downgrade even after quarantine recovery.
- Exact artifact hashes, original failed logs and focused rechecks are retained privately under
  `out/combined-release-20261102/` and must be checked before deployment.

Independent source review accepted the bounded immunity repair, retained quest transaction and
native schedule. These results are synthetic/component or TLS integration evidence. No new
original-client gameplay acceptance is claimed. Corpse gathering hold-F behavior is excluded.

The canonical cloud Page could not accept the earlier update because its document size limit
was reached. Its last confirmed publication fingerprint must remain unchanged until a guarded
write and readback succeed; local reference regeneration does not imply Page synchronization.
