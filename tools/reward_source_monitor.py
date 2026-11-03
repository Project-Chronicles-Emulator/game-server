"""Sample aggregate reward and gathering health without sending notifications.

Connection settings come from libpq's environment or service/passfile; credentials must
not be supplied as arguments. Run this under a dedicated PostgreSQL read-only role.
"""

import argparse
import json
import math
import os
from pathlib import Path
import re
import secrets
import subprocess
import sys
import tempfile
import time


# Keep read-only monitoring queries here; installation has one SQL file.
CHECKS = {
    "monster": """
-- Read-only capacity and alert report; no character or receipt identifiers are returned.
WITH actual AS (
  SELECT world_id, count(*) AS actual_event_count,
         count(*) FILTER (WHERE NOT ready) AS staged_events
  FROM emulator_monster_reward_events GROUP BY world_id
), backlog AS (
  SELECT world_id, count(*) AS characters_with_pending,
         sum(per_character) AS pending_receipts, max(per_character) AS largest_character_backlog
  FROM (SELECT world_id, character_id, count(*) AS per_character
        FROM emulator_monster_reward_receipts WHERE NOT processed
        GROUP BY world_id, character_id) AS characters
  GROUP BY world_id
)
SELECT COALESCE(limits.world_id, actual.world_id) AS world_id,
       limits.event_count AS retained_events,
       1000000 - limits.event_count AS remaining_event_capacity,
       COALESCE(actual.actual_event_count, 0) AS actual_event_count,
       COALESCE(actual.staged_events, 0) AS staged_events,
       COALESCE(backlog.pending_receipts, 0) AS pending_receipts,
       COALESCE(backlog.largest_character_backlog, 0) AS largest_character_backlog,
       CASE
         WHEN limits.event_count IS NULL OR limits.event_count <> COALESCE(actual.actual_event_count, 0)
           THEN 'COUNTER_MISMATCH'
         WHEN limits.event_count >= 900000 OR COALESCE(backlog.largest_character_backlog, 0) >= 922
           THEN 'CRITICAL'
         WHEN limits.event_count >= 800000 OR COALESCE(backlog.largest_character_backlog, 0) >= 819
           THEN 'WARNING'
         ELSE 'OK'
       END AS capacity_status
FROM emulator_monster_reward_limits AS limits
FULL JOIN actual ON actual.world_id = limits.world_id
LEFT JOIN backlog ON backlog.world_id = COALESCE(limits.world_id, actual.world_id)
ORDER BY world_id
""",
    "quest": """
-- Read-only aggregates, without character or operation identities. The oldest age is a
-- lower bound for rows backfilled by the timestamp migration. Future timestamps indicate
-- clock skew and must be investigated rather than treated as healthy zero-age entries.
-- Proposed operations policy: pending for 15 minutes / 1 hour / 4 hours is
-- warning / urgent / critical respectively. Thresholds do not provide delivery by themselves.
WITH event_counts AS (
  SELECT world_id, count(*) AS actual_events,
         count(*) FILTER (WHERE NOT company_applied OR NOT guild_applied) AS pending_events,
         min(created_at) FILTER (WHERE NOT company_applied OR NOT guild_applied) AS oldest_pending,
         count(*) FILTER (WHERE created_at > statement_timestamp()) AS future_timestamps
  FROM emulator_quest_service_effects
  GROUP BY world_id
), pending_per_character AS (
  SELECT world_id, character_id, count(*) AS pending
  FROM emulator_quest_service_effects
  WHERE NOT company_applied OR NOT guild_applied
  GROUP BY world_id, character_id
), pending_peaks AS (
  SELECT world_id, max(pending) AS max_character_pending
  FROM pending_per_character
  GROUP BY world_id
), summary AS (
  SELECT COALESCE(limits.world_id, events.world_id) AS world_id,
         limits.event_count AS recorded_events,
         COALESCE(events.actual_events, 0) AS actual_events,
         COALESCE(events.pending_events, 0) AS pending_events,
         events.oldest_pending,
         COALESCE(events.future_timestamps, 0) AS future_timestamps,
         COALESCE(peaks.max_character_pending, 0) AS max_character_pending
  FROM emulator_quest_service_effect_limits AS limits
  FULL JOIN event_counts AS events ON events.world_id = limits.world_id
  LEFT JOIN pending_peaks AS peaks ON peaks.world_id = COALESCE(limits.world_id, events.world_id)
)
SELECT world_id, recorded_events, actual_events, 1000000 AS world_capacity,
       pending_events, max_character_pending, 1024 AS per_character_capacity,
       oldest_pending, future_timestamps,
       CASE
         WHEN recorded_events IS NULL OR recorded_events <> actual_events OR future_timestamps > 0
           OR actual_events >= 950000 OR max_character_pending >= 973
           OR oldest_pending <= statement_timestamp() - interval '4 hours' THEN 'critical'
         WHEN actual_events >= 850000 OR max_character_pending >= 870
           OR oldest_pending <= statement_timestamp() - interval '1 hour' THEN 'urgent'
         WHEN actual_events >= 700000 OR max_character_pending >= 717
           OR oldest_pending <= statement_timestamp() - interval '15 minutes' THEN 'warning'
         ELSE 'ok'
       END AS severity
FROM summary
ORDER BY world_id
""",
    "loot": """
-- Read-only aggregate capacity report; no player or receipt identifiers are emitted.
WITH actual AS (
  SELECT world_id, count(*) AS actual_receipts,
         count(*) FILTER (WHERE NOT terminal) AS unfinished_gathering
  FROM emulator_loot_claim_receipts GROUP BY world_id
)
SELECT COALESCE(limits.world_id, actual.world_id) AS world_id,
       limits.receipt_count AS recorded_receipts,
       COALESCE(actual.actual_receipts, 0) AS actual_receipts,
       1000000 AS world_capacity,
       COALESCE(actual.unfinished_gathering, 0) AS unfinished_gathering,
       CASE
         WHEN limits.receipt_count IS NULL OR
              limits.receipt_count <> COALESCE(actual.actual_receipts, 0) OR
              limits.receipt_count >= 950000 THEN 'critical'
         WHEN limits.receipt_count >= 850000 THEN 'urgent'
         WHEN limits.receipt_count >= 700000 THEN 'warning'
         ELSE 'ok'
       END AS severity
FROM emulator_loot_claim_limits AS limits
FULL JOIN actual ON actual.world_id = limits.world_id
ORDER BY world_id
""",
    "sources": """
-- Read-only, per-world aggregate. Counter drift indicates unsupported writes or corruption.
WITH actual AS (
  SELECT world_id, count(*) AS actual_sources,
         count(*) FILTER (WHERE pending_receipt_id IS NOT NULL) AS unfinished_sources
  FROM emulator_gathering_sources
  GROUP BY world_id
)
SELECT COALESCE(limits.world_id, actual.world_id) AS world_id,
       limits.source_count AS recorded_sources,
       COALESCE(actual.actual_sources, 0) AS actual_sources,
       1000000 AS world_capacity,
       COALESCE(actual.unfinished_sources, 0) AS unfinished_sources,
       CASE
         WHEN limits.source_count IS NULL OR
              limits.source_count <> COALESCE(actual.actual_sources, 0) OR
              limits.source_count >= 950000 THEN 'critical'
         WHEN limits.source_count >= 850000 THEN 'urgent'
         WHEN limits.source_count >= 700000 THEN 'warning'
         ELSE 'ok'
       END AS severity
FROM emulator_gathering_source_limits AS limits
FULL JOIN actual ON actual.world_id = limits.world_id
ORDER BY world_id
""",
    "boss": """
-- Aggregate boss-script queue only; no event IDs, character data or copy scopes are returned.
SELECT world_id,
       count(*) FILTER (WHERE NOT boss_script_published AND NOT ready) AS staged_scripts,
       count(*) FILTER (WHERE NOT boss_script_applied AND ready) AS unsettled_scripts,
       count(*) FILTER (WHERE boss_script_applied AND NOT boss_script_published AND ready)
         AS awaiting_publication,
       count(*) FILTER (WHERE (NOT boss_script_published AND boss_script IS NULL) OR
                              (boss_script_published AND NOT boss_script_applied)) AS malformed_scripts
FROM emulator_monster_reward_events
GROUP BY world_id
ORDER BY world_id
""",
}
SEVERITY = {"ok": 0, "warning": 1, "urgent": 2, "critical": 3}
MAX_OUTPUT_BYTES = 1048576
MAX_WORLDS = 128
CLOCK_TOLERANCE_SECONDS = 60
REMINDER_SECONDS = 3600
MAX_OUTBOX_EVENTS = 1024
TABLES = (
    "emulator_schema_version",
    "emulator_monster_reward_events",
    "emulator_monster_reward_receipts",
    "emulator_monster_reward_limits",
    "emulator_quest_service_effects",
    "emulator_quest_service_effect_limits",
    "emulator_loot_claim_receipts",
    "emulator_loot_claim_limits",
    "emulator_gathering_sources",
    "emulator_gathering_source_limits",
)
EMPTY_REPORTS = {
    "monster": {"retained_events": 0, "actual_event_count": 0, "staged_events": 0,
                "pending_receipts": 0, "largest_character_backlog": 0, "capacity_status": "OK"},
    "quest": {"recorded_events": 0, "actual_events": 0, "pending_events": 0,
              "max_character_pending": 0, "oldest_pending": None, "future_timestamps": 0,
              "severity": "ok"},
    "loot": {"recorded_receipts": 0, "actual_receipts": 0, "unfinished_gathering": 0,
             "severity": "ok"},
    "sources": {"recorded_sources": 0, "actual_sources": 0, "unfinished_sources": 0,
                "severity": "ok"},
    "boss": {"staged_scripts": 0, "unsettled_scripts": 0, "awaiting_publication": 0,
             "malformed_scripts": 0},
}


def sql_program(worlds, timeout_ms):
    world_list = ", ".join(str(world) for world in worlds)
    statements = [
        "BEGIN TRANSACTION ISOLATION LEVEL REPEATABLE READ READ ONLY;",
        "SET LOCAL search_path = pg_catalog, public;",
        f"SET LOCAL statement_timeout = {timeout_ms};",
        "SET LOCAL lock_timeout = 1000;",
        "SELECT json_build_object('section','identity', 'database', current_database(), "
        "'epoch', extract(epoch FROM clock_timestamp()), "
        "'version', (SELECT max(version) FROM public.emulator_schema_version), "
        "'versions', (SELECT count(*) FROM public.emulator_schema_version "
        "WHERE version IN (39,41,42,43,44)))::text;",
        "SELECT json_build_object('section','privileges', 'unsafe', EXISTS ("
        "SELECT 1 FROM pg_roles role WHERE pg_has_role(current_user, role.oid, 'MEMBER') "
        "AND (role.rolsuper OR role.rolcreaterole OR role.rolcreatedb OR role.rolreplication "
        "OR role.rolbypassrls OR has_database_privilege(role.oid, current_database(), 'CREATE,TEMPORARY') "
        "OR EXISTS (SELECT 1 FROM pg_database d WHERE d.datname = current_database() AND d.datdba = role.oid) "
        "OR EXISTS (SELECT 1 FROM pg_namespace n WHERE n.nspname NOT LIKE 'pg_%' "
        "AND n.nspname <> 'information_schema' AND (n.nspowner = role.oid "
        "OR has_schema_privilege(role.oid, n.oid, 'CREATE'))) "
        "OR EXISTS (SELECT 1 FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
        "WHERE n.nspname NOT LIKE 'pg_%' AND n.nspname <> 'information_schema' "
        "AND ((c.relkind IN ('r','p','v','m','f') AND (c.relowner = role.oid "
        "OR has_table_privilege(role.oid, c.oid, 'INSERT,UPDATE,DELETE,TRUNCATE,TRIGGER,REFERENCES') "
        "OR has_any_column_privilege(role.oid, c.oid, 'INSERT,UPDATE,REFERENCES'))) "
        "OR (c.relkind = 'S' AND (c.relowner = role.oid "
        "OR has_sequence_privilege(role.oid, c.oid, 'USAGE,UPDATE'))))))))::text;",
    ]
    for name, query in CHECKS.items():
        query = query.strip()
        statements.append(
            f"SELECT json_build_object('section','{name}', 'rows', "
            f"COALESCE(json_agg(report ORDER BY world_id), '[]'::json))::text "
            f"FROM ({query}) AS report WHERE world_id IN ({world_list});"
        )
    statements.append("COMMIT;")
    return "\n".join(statements)


def sample(psql, database, worlds, timeout_seconds):
    started = time.time()
    with tempfile.TemporaryFile() as output:
        try:
            result = subprocess.run(
                [str(psql), "-X", "-q", "-A", "-t", "-v", "ON_ERROR_STOP=1", "-d", database],
                input=sql_program(worlds, int(timeout_seconds * 1000)),
                text=True,
                stdout=output,
                stderr=subprocess.DEVNULL,
                timeout=timeout_seconds + 5,
                check=False,
            )
        except (OSError, subprocess.TimeoutExpired) as error:
            raise RuntimeError("database query unavailable or timed out") from error
        finished = time.time()
        if result.returncode != 0:
            raise RuntimeError("database query failed; check schema, permissions and connectivity")
        if output.tell() > MAX_OUTPUT_BYTES:
            raise RuntimeError("database result exceeded the output limit")
        output.seek(0)
        try:
            documents = [json.loads(line) for line in output.read().decode("utf-8").splitlines()]
            sections = {document["section"]: document for document in documents}
        except (UnicodeError, ValueError, KeyError, TypeError) as error:
            raise RuntimeError("database returned malformed results") from error
    if len(documents) != len(CHECKS) + 2 or set(sections) != set(CHECKS) | {"identity", "privileges"}:
        raise RuntimeError("database returned incomplete or duplicate results")
    identity = sections["identity"]
    if identity.get("database") != database or identity.get("version") != 44 or identity.get("versions") != 5:
        raise RuntimeError("database identity or schema version does not match")
    if sections["privileges"].get("unsafe") is not False:
        raise RuntimeError("monitor role has elevated or write privileges")
    epoch = float(identity["epoch"])
    if not math.isfinite(epoch) or not started - CLOCK_TOLERANCE_SECONDS <= epoch <= finished + CLOCK_TOLERANCE_SECONDS:
        raise RuntimeError("database and host clocks differ beyond the allowed tolerance")
    return sections


def findings(sections, worlds, previous_streaks):
    result = {}
    streaks = {}
    for world in worlds:
        for name in CHECKS:
            rows = sections[name].get("rows")
            if not isinstance(rows, list) or any(not isinstance(row, dict) for row in rows):
                raise RuntimeError("database report is malformed")
            matches = [row for row in rows if row.get("world_id") == world]
            if len(matches) > 1:
                raise RuntimeError("database report contains duplicate worlds")
            row = matches[0] if matches else {"world_id": world, **EMPTY_REPORTS[name]}
            for field, default in EMPTY_REPORTS[name].items():
                value = row.get(field)
                missing_counter = field in {"retained_events", "recorded_events", "recorded_receipts",
                                            "recorded_sources"} and value is None and field in row
                if field not in row or (isinstance(default, int) and
                                        not missing_counter and (type(value) is not int or value < 0)):
                    raise RuntimeError("database report is incomplete or invalid")
            status = row.get("capacity_status", row.get("severity", "ok"))
            if not isinstance(status, str):
                raise RuntimeError("database report has an invalid severity")
            status = status.lower()
            if status == "counter_mismatch":
                status = "critical"
            if status not in SEVERITY:
                raise RuntimeError("database report has an unknown severity")
            if name == "boss" and row.get("malformed_scripts", 0) > 0:
                status = "critical"
            if name == "boss" and any(row.get(key, 0) > 0 for key in (
                "staged_scripts", "unsettled_scripts", "awaiting_publication"
            )) and SEVERITY[status] < SEVERITY["warning"]:
                status = "warning"
            # These are sustained aggregate counts, not proof that a particular row is old.
            observed_backlog = (
                (name == "monster" and row.get("staged_events", 0) > 0)
                or (name == "loot" and row.get("unfinished_gathering", 0) > 0)
                or (name == "sources" and row.get("unfinished_sources", 0) > 0)
                or (name == "boss" and any(row.get(key, 0) > 0 for key in (
                    "staged_scripts", "unsettled_scripts", "awaiting_publication"
                )))
            )
            key = f"{world}:{name}"
            if observed_backlog:
                streaks[key] = min(previous_streaks.get(key, 0) + 1, 48)
                if streaks[key] >= 48:
                    status = "critical"
                elif streaks[key] >= 12 and SEVERITY[status] < SEVERITY["urgent"]:
                    status = "urgent"
                elif streaks[key] >= 3 and SEVERITY[status] < SEVERITY["warning"]:
                    status = "warning"
                row["consecutive_nonzero_samples"] = streaks[key]
            result[key] = {"severity": status, "metrics": row}
    return result, streaks


def event_changes(previous, current, now):
    notifications = []
    updated = {}
    for key in sorted(set(previous) | set(current)):
        old = previous.get(key, {})
        new = current.get(key, {"severity": "ok"})
        severity = new["severity"]
        old_severity = old.get("severity", "ok")
        last_notice = old.get("last_notice", 0)
        changed = severity != old_severity
        reminder = severity != "ok" and now - last_notice >= REMINDER_SECONDS
        if changed or reminder:
            notifications.append({"key": key, "severity": severity, "metrics": new.get("metrics", {})})
            last_notice = now
        if severity != "ok":
            updated[key] = {"severity": severity, "last_notice": last_notice}
    return updated, notifications


def validate_state(state, database, worlds):
    if not isinstance(state, dict):
        raise RuntimeError("invalid monitor state")
    if state.get("format") != 2 or state.get("database") != database or state.get("worlds") != worlds:
        raise RuntimeError("monitor state identity or format does not match")
    active = state.get("active")
    streaks = state.get("streaks")
    outbox = state.get("outbox")
    if not isinstance(active, dict) or not isinstance(streaks, dict) or not isinstance(outbox, list):
        raise RuntimeError("invalid monitor state")
    valid_keys = {f"{world}:{name}" for world in worlds for name in CHECKS} | {"monitor"}
    if not set(active).issubset(valid_keys) or not set(streaks).issubset(valid_keys):
        raise RuntimeError("invalid monitor state")
    for value in active.values():
        if (not isinstance(value, dict) or not isinstance(value.get("severity"), str)
                or value.get("severity") not in SEVERITY
                or value.get("severity") == "ok" or type(value.get("last_notice")) is not int
                or value["last_notice"] < 0):
            raise RuntimeError("invalid monitor state")
    if any(type(value) is not int or not 1 <= value <= 48 for value in streaks.values()):
        raise RuntimeError("invalid monitor state")
    if len(outbox) > MAX_OUTBOX_EVENTS:
        raise RuntimeError("invalid monitor state")
    ids = set()
    for event in outbox:
        if (not isinstance(event, dict) or set(event) != {"id", "key", "severity"}
                or not isinstance(event.get("id"), str)
                or not re.fullmatch(r"[0-9a-f]{32}", event["id"])
                or not isinstance(event.get("key"), str) or event.get("key") not in valid_keys
                or not isinstance(event.get("severity"), str) or event.get("severity") not in SEVERITY
                or event["id"] in ids):
            raise RuntimeError("invalid monitor state")
        ids.add(event["id"])
    last_acked = state.get("last_acked")
    if "last_acked" not in state or (last_acked is not None and (not isinstance(last_acked, str)
                                   or not re.fullmatch(r"[0-9a-f]{32}", last_acked)
                                   or last_acked in ids)):
        raise RuntimeError("invalid monitor state")
    for field in ("last_attempt", "consecutive_failures"):
        if type(state.get(field)) is not int or state[field] < 0:
            raise RuntimeError("invalid monitor state")
    last_success = state.get("last_success")
    if "last_success" not in state or (last_success is not None and
                                      (type(last_success) is not int or last_success < 0)):
        raise RuntimeError("invalid monitor state")
    return state


def write_state(path, state):
    encoded = (json.dumps(state, sort_keys=True) + "\n").encode("utf-8")
    if len(encoded) > MAX_OUTPUT_BYTES:
        raise RuntimeError("monitor state exceeds the size limit")
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("wb") as handle:
        handle.write(encoded)
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(temporary, path)
    if os.name != "nt":
        descriptor = os.open(path.parent, os.O_RDONLY)
        try:
            os.fsync(descriptor)
        finally:
            os.close(descriptor)


def head_event(outbox):
    return outbox[:1]


def lock_file(handle):
    if os.name == "nt":
        import msvcrt

        handle.seek(0)
        if not handle.read(1):
            handle.write(b"0")
            handle.flush()
        handle.seek(0)
        msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
    else:
        import fcntl

        fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--psql", required=True, type=Path)
    parser.add_argument("--database", required=True)
    parser.add_argument("--world", required=True, action="append", type=int)
    parser.add_argument("--state", required=True, type=Path)
    parser.add_argument("--timeout-seconds", type=int, default=15, choices=range(1, 61))
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--ack", help="acknowledge the oldest event after confirmed delivery")
    mode.add_argument("--pending", action="store_true", help="read the oldest pending event without sampling")
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_]{1,63}", args.database):
        parser.error("database must be a simple database name, not a connection string")
    worlds = sorted(set(args.world))
    if len(worlds) > MAX_WORLDS or any(world <= 0 or world > 2147483647 for world in worlds):
        parser.error("world IDs must be positive 32-bit integers, at most 128 worlds")
    try:
        args.state.parent.mkdir(parents=True, exist_ok=True)
        with args.state.with_suffix(args.state.suffix + ".lock").open("a+b") as lock:
            lock_file(lock)
            previous = {}
            if args.state.exists():
                if args.state.stat().st_size > MAX_OUTPUT_BYTES:
                    raise RuntimeError("monitor state exceeds the size limit")
                previous = validate_state(json.loads(args.state.read_text(encoding="utf-8")),
                                          args.database, worlds)
            if args.pending or args.ack is not None:
                if not previous:
                    raise RuntimeError("monitor state does not exist")
                if args.ack is not None:
                    if not re.fullmatch(r"[0-9a-f]{32}", args.ack):
                        raise RuntimeError("invalid event acknowledgement")
                    outbox = previous["outbox"]
                    if outbox and outbox[0]["id"] == args.ack:
                        previous["last_acked"] = args.ack
                        del outbox[0]
                        write_state(args.state, previous)
                    elif previous["last_acked"] != args.ack:
                        raise RuntimeError("event acknowledgement is out of order")
                print(json.dumps({"status": "pending", "events": head_event(previous["outbox"])}))
                return 0
            now = int(time.time())
            try:
                current, streaks = findings(
                    sample(args.psql, args.database, worlds, args.timeout_seconds), worlds,
                    previous.get("streaks", {}),
                )
                error = None
            except (RuntimeError, KeyError, TypeError, ValueError) as failure:
                current = {
                    key: {"severity": value["severity"]}
                    for key, value in previous.get("active", {}).items()
                }
                current["monitor"] = {"severity": "critical", "metrics": {"reason": str(failure)}}
                streaks = previous.get("streaks", {})
                error = True
            active, events = event_changes(previous.get("active", {}), current, now)
            outbox = list(previous.get("outbox", []))
            if len(outbox) + len(events) > MAX_OUTBOX_EVENTS:
                raise RuntimeError("monitor event outbox is full")
            for event in events:
                event_id = secrets.token_hex(16)
                if any(queued["id"] == event_id for queued in outbox) or event_id == previous.get("last_acked"):
                    raise RuntimeError("monitor event ID collision")
                outbox.append({"id": event_id, "key": event["key"],
                               "severity": event["severity"]})
            state = {
                "format": 2,
                "database": args.database,
                "worlds": worlds,
                "active": active,
                "streaks": streaks,
                "outbox": outbox,
                "last_acked": previous.get("last_acked"),
                "last_attempt": now,
                "last_success": previous.get("last_success") if error else now,
                "consecutive_failures": previous.get("consecutive_failures", 0) + 1 if error else 0,
            }
            write_state(args.state, state)
            print(json.dumps({"status": "failed" if error else "sampled", "heartbeat": now,
                              "reports": {} if error else current, "events": head_event(outbox)},
                             sort_keys=True))
            return 2 if error else 0
    except RuntimeError as error:
        print(json.dumps({"status": "failed", "reason": str(error)}))
        return 2
    except (OSError, ValueError):
        print(json.dumps({"status": "failed", "reason": "monitor state or lock unavailable"}))
        return 2


if __name__ == "__main__":
    sys.exit(main())
