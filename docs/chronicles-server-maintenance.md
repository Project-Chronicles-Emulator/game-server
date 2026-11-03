# Project Chronicles server maintenance

The human authorized a dedicated server-maintainer on 2026-10-25. It runs
Claude Sonnet 5.5 at high thinking in OMP. Its only responsibility is keeping
the established emulator server on the latest validated remote default-branch
revision. The current verified default is `origin/main`, not a branch named
`master`. Confirm that ref before each delivery.

## Ownership and execution

```text
Remote default-branch commit
  -> server-maintainer: inspect exact revision and reserve the service
  -> integrator/maintainer: explicit handoff of the single build slot
  -> candidate build and validation in a separate output directory
  -> authenticated database backup, restore listing and artifact hashes
  -> server-maintainer: stop owned PID, migrate, switch and check routes/data
  -> Claude reviewer: independently check the running revision and behavior
  -> deployment record, or rollback to the prior working binary/config/data
```

Builders integrate source and prepare reviewed commits. They hand deployment
to server-maintainer rather than restarting the live server themselves. The
maintainer does not implement game features or approve its own deployment.
All claims and build/service locks apply across both Claude and SWE groups.

The standing monitor checks the remote ref and actual deployment record on
each run. When a newer revision is available, it queues a bounded maintenance
assignment after the relevant ownership and live-job checks. The maintainer
does not create another polling loop or scheduler. Manual Stop/Pause prevents
automatic continuation and never authorizes replay of a canceled operation.

The established root startup helper and persisted PostgreSQL settings are the
deployment contract. Verify the current process path, executable hash, ports
and database identity before any restart. Preserve unrelated services and
dirty source checkouts. Reuse a suitable deployment checkout; do not reset a
feature worktree or overwrite a running executable.

## Necessary data resets

The human explicitly allowed closing the server and wiping data if necessary
to keep it current. Prefer supported additive migrations and normal upgrades.
A reset is authorized only for the established emulator database or its owned
incompatible generated state, after identifying the actual incompatibility.
It is not a general cleanup instruction and does not include other services,
source/capture/IDA evidence, credentials, backups or arbitrary directories.

Before a reset, stop the owned service, record the exact target and reason,
make an authenticated backup, verify nonzero size/hash/restore listing, and
retain a usable restoration path outside the reset target. Record which
emulator player/account/progress data will be erased. Missing backup, unknown
database ownership or an unexplained failure is a blocker, not a reason to
wipe. Do not silently reset data for a source bug or avoidable migration error.

If deployment fails, restore the previous binary and configuration, and restore
the pre-reset database when required. Independently verify the previous service
is healthy before reporting rollback success. Preserve the backup afterward.

## Evidence

Use `out/agent-team-reports/server-maintainer/` for private deployment records.
Include remote revision, candidate/running binary hash, verified PID/ports,
backup path/hash/list result, migration/reset scope, route/data/clock checks,
independent reviewer result and rollback outcome. Never record credentials or
private packet/player values in public documentation.

Being current with `origin/main` does not establish original-client gameplay
acceptance. Combat and other features remain unfinished until their required
behavior and client checks pass.
