# Reward and gathering monitor candidate

This is an operator tool, not an installed monitor. It prints one JSON result per run
and does not send messages, create a scheduled task, or configure a heartbeat service.
Choose and test an alert destination and an independent dead-man heartbeat before
claiming operational coverage.

## Sampling

Run `tools/reward_source_monitor.py` using a dedicated PostgreSQL read-only login and
libpq service/passfile settings. Provide the exact simple database name, each intended
world ID, the PostgreSQL `psql` executable, and a state path restricted to the monitor
identity and operators. Never put credentials or connection URLs in arguments. For
example, after configuring an approved libpq service with verified TLS and server
identity:

```powershell
$env:PGSERVICE = 'chronicles-monitor'
python tools/reward_source_monitor.py --psql 'C:\PostgreSQL\bin\psql.exe' `
  --database 'game_database' --world 1 --state 'C:\monitor-state\rewards.json'
```

Use the actual approved executable, service, database, worlds and secured state
location; these values are examples. Libpq TLS/host verification and the service's
database/host mapping require independent operator verification. The monitor checks the
database name and exact schema 44, but cannot establish a cluster's identity from its
name. Schema 39, 41, 42, 43 and 44 version records must all exist. The reports run in
one repeatable-read, read-only transaction with a one-second lock timeout, configurable
1–60 second statement timeout and a whole-process timeout. A failed query, invalid
result, missing schema, excessive host/database clock difference or unsafe role is a
monitor failure, never an empty or healthy sample. Raw PostgreSQL errors and connection
settings are not printed. SQL reports may scan retained rows; measure their latency
against representative production-scale data and set an appropriate timeout before
enabling a schedule.

The result contains aggregate `reports`, alert `events`, and a `heartbeat` timestamp.
Only aggregate counts, world IDs and the v41 oldest-pending timestamp are emitted; it
does not emit player, character or receipt IDs. A successful empty world is represented
by zeros, after the schema and report queries succeed. Exit code 0 means sampling
succeeded, including when a report is critical; exit code 2 means sampling or monitor
state failed. A router must evaluate the event severities and exit status independently.
State records the last success, consecutive failures, active alerts, and backlog sample
streaks, plus a durable ordered event outbox; state changes use a flushed atomic replace
under a nonblocking lock. Use one unique
state path per database and world set. A changed scope fails closed rather than silently
clearing alerts. Lock or state failure emits a generic error and exits 2; it cannot
reliably update the state file.

The existing versioned SQL checks define capacity and consistency severity. v39 uses
80% warning and 90% critical. v41–v43 use 70% warning, 85% urgent, and 95% critical.
Counter drift is critical. v41 pending age uses 15 minutes warning, one hour urgent,
and four hours critical; migrated old rows have a lower-bound age. Future v41 timestamps
are critical. A host/database clock difference over 60 seconds fails the sample.
Staged monster work, unfinished gathering claims/sources and unpublished boss work
have no trusted per-item age. Three, 12 and 48 consecutive nonzero samples escalate
their aggregate signal to warning, urgent and critical; boss work is already warning
at the first nonzero sample. Counts remaining nonzero do not establish that the same
individual work item is stuck. Missing or failed samples do not reset a streak or clear
active gameplay alerts. Severity changes and recovery emit events, unchanged alerts are
deduplicated, and active alerts are queued again after one hour. The first pending
event is returned on every run until the router confirms delivery and explicitly
acknowledges its event ID. After actual delivery, invoke the same command with
`--ack <event-id>`; its output contains the next event, if any. A lost acknowledgement
response can be retried for the most recently acknowledged ID. `--pending` reads the
head without querying the database. Acknowledgements must be in order. An alert followed
by recovery remains two ordered events even if the router was unavailable throughout.
Routers must deduplicate by event ID: a crash after remote delivery but before local
acknowledgement necessarily causes the event to be offered again. Do not acknowledge on
an attempted send or an unconfirmed response. A full 1,024-event outbox fails the run
without overwriting pending state; drain it with `--pending` and confirmed `--ack` before
resuming sampling. A state failure must be reported independently because it cannot be
queued reliably. State loss can lose pending events and streak history; protect and back
up the state and monitor its availability. The atomic file protocol is tested for process
restart; power-loss durability on a particular Windows filesystem is not established.

For a proposed five-minute schedule, use an independent supervisor and alert if no
successful sample arrives within 15 minutes; test its failure and recovery paths. A
heartbeat that treats a failed sample as success would hide monitoring failure. System
clock adjustments affect age and reminder timing and require investigation. Neither
the task nor an external heartbeat is installed by this tool.

## Optional Windows Event Log routing

`tools/reward_monitor_eventlog.py` is an uninstalled adapter. Its `sample` mode runs the
same monitor with the same arguments, writes each pending alert to the Windows Application
log under the pre-registered `ChroniclesRewardMonitor` source, and acknowledges the outbox head
only after Windows accepts the event. It sends at most 32 alerts per run and fails if more
remain. A crash between event acceptance and acknowledgement can repeat an event: consumers
must deduplicate by the event ID in its JSON insertion string. Windows accepting an event
does not prove that a collector received it or that it survives log rotation or host failure.

The event IDs are 100 for a successful sample, 101 for a sampling failure, 102/103/104 for
warning/urgent/critical, 105 for recovery, and 106 for a stale or invalid heartbeat. Event
messages contain an opaque scope hash and, for alerts, the event ID, world/report key and
severity. They do not contain connection settings, credentials or player identifiers.
The scope hash distinguishes the configured database name and world IDs, but does not prove
which PostgreSQL cluster is being queried.

Only after the sample succeeds, every pending alert for the run is routed and acknowledged,
and Windows accepts event 100 does `sample` atomically update a separate restricted
heartbeat file alongside the monitor state. Failed samples, routing, acknowledgements or
Event Log writes do not refresh it. Launch, timeout, malformed-output and state-lock failures
also attempt to emit event 101; if Event Log itself is unavailable the process exits nonzero
and writes a generic error to standard error. Run `watchdog` independently with the same
database, worlds and state path; it writes
event 106 and exits 2 when the heartbeat is missing, invalid, in the future or older than
`--max-age-seconds` (default 900). Both the sampler and watchdog must be supervised for
nonzero exits, including Event Log failures. A same-host watchdog cannot report a stopped
host or broken Event Log; an independently monitored collector and dead-man heartbeat are
still required. Back up and protect the state and heartbeat files, and use only one router
for a given outbox. Check collector subscriptions, log retention, event rendering and
deduplication on the actual Windows host before relying on this option.

The following examples do not install anything. Replace the paths and establish the
restricted libpq service as described above:

```powershell
$env:PGSERVICE = 'chronicles-monitor'
python tools/reward_monitor_eventlog.py sample --psql 'C:\PostgreSQL\bin\psql.exe' `
  --database 'game_database' --world 1 --state 'C:\monitor-state\rewards.json'
python tools/reward_monitor_eventlog.py watchdog --psql 'C:\PostgreSQL\bin\psql.exe' `
  --database 'game_database' --world 1 --state 'C:\monitor-state\rewards.json'
```

An administrator must separately register and validate the Application event source and
its message resource before the least-privilege task runs; the adapter refuses an absent
source and never registers one. Use a dedicated task identity with only Event Log write,
read access to its interpreter/scripts/libpq configuration and required monitor queries,
and write access to its own protected state directory. The independent watcher needs only
heartbeat read access and Event Log write access. Validate the actual Windows ACLs and
database grants, assign an alert owner and test a separate collector, scheduling, failed
sampling, stale heartbeat and host-loss behavior before claiming operational coverage.
The mock-based router tests run with `python tools/test_reward_monitor_eventlog.py`; they
do not write to the actual Event Log or test Windows source registration.

## Database role and permissions

Use a separate non-owner login with schema USAGE and only the following column SELECT
permissions. Provisioning and changes to PUBLIC privileges must be reviewed for the
specific database; the disposable test revokes PUBLIC schema CREATE and database TEMP.

| Table | SELECT columns |
| --- | --- |
| `emulator_schema_version` | `version` |
| `emulator_monster_reward_events` | `world_id`, `ready`, `boss_script`, `boss_script_applied`, `boss_script_published` |
| `emulator_monster_reward_receipts` | `world_id`, `character_id`, `processed` |
| `emulator_monster_reward_limits` | `world_id`, `event_count` |
| `emulator_quest_service_effects` | `world_id`, `character_id`, `created_at`, `company_applied`, `guild_applied` |
| `emulator_quest_service_effect_limits` | `world_id`, `event_count` |
| `emulator_loot_claim_receipts` | `world_id`, `terminal` |
| `emulator_loot_claim_limits` | `world_id`, `receipt_count` |
| `emulator_gathering_sources` | `world_id`, `pending_receipt_id` |
| `emulator_gathering_source_limits` | `world_id`, `source_count` |

Some columns required to aggregate per-character backlog and validate boss payloads
can themselves contain identifiers. The tool never prints them, but direct SQL access
by this login can read them; restrict credential and host access accordingly. A reviewed
aggregate-only reporting view or service is needed if direct identifier access is
unacceptable.

The tool rejects roles or memberships with superuser, role/database creation,
replication, bypass-RLS, database CREATE/TEMP, ownership, schema CREATE, table/column
write or sequence mutation privileges, including inherited grants. This is a useful
guard, not a complete security audit: review executable functions, security-definer
procedures, role options, database ownership, grants outside the inspected database,
and host access independently. Test actual SELECT success and write, DDL, TEMP and
role-escalation denial with the final production role; never use the application login.

## Response and recovery

On monitor or heartbeat failure, restore observation first and inspect sanitized
operator logs, PostgreSQL availability, schema, grants and clock. On capacity, age,
backlog or counter alerts, inspect system health and follow reviewed recovery procedures;
do not edit rows or counters merely to silence an alert. Never compact a pending
receipt or active source. Compaction requires a verified quiescent backup and proof
that every application, client, detached recovery and maintenance retry source has
stopped. Schema-44 downgrade is fail-closed; the rehearsed fallback is restoring a
verified pre-release backup, which loses subsequent player changes unless separately
reconciled and approved.

The isolated regression command is `python tools/test_reward_source_monitor.py
<postgres-bin-directory>`. It creates and stops its own disposable loopback cluster and
does not touch the live server. Its local results do not validate live alert routing,
production query latency, operating-system ACLs, TLS identity or original-client play.
