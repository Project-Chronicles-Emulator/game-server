"""Optional Windows Event Log router for the reward/source monitor.

This tool does not register an event source, install a task, or configure alerts.
"""

import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time

import reward_source_monitor as monitor


SOURCE = "ChroniclesRewardMonitor"
SUCCESS_EVENT = 100
FAILURE_EVENT = 101
STALE_EVENT = 106
EVENT_IDS = {"warning": 102, "urgent": 103, "critical": 104, "ok": 105}
EVENT_TYPES = {"warning": 2, "urgent": 1, "critical": 1, "ok": 4}
MAX_EVENTS_PER_RUN = 32
MAX_ROUTER_OUTPUT = 2 * 1024 * 1024


def scope_id(database, worlds):
    scope = json.dumps([database, sorted(set(worlds))], separators=(",", ":"))
    return hashlib.sha256(scope.encode("utf-8")).hexdigest()[:16]


def write_event(event_id, event_type, message):
    if os.name != "nt":
        raise RuntimeError("Windows Event Log is unavailable")

    import winreg

    key_path = "SYSTEM\\CurrentControlSet\\Services\\EventLog\\Application\\" + SOURCE
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, key_path) as key:
            winreg.QueryValueEx(key, "EventMessageFile")
    except OSError as error:
        raise RuntimeError("Event Log source is not registered") from error

    library = ctypes.WinDLL("Advapi32", use_last_error=True)
    library.RegisterEventSourceW.argtypes = (wintypes.LPCWSTR, wintypes.LPCWSTR)
    library.RegisterEventSourceW.restype = wintypes.HANDLE
    library.ReportEventW.argtypes = (
        wintypes.HANDLE, wintypes.WORD, wintypes.WORD, wintypes.DWORD,
        wintypes.LPVOID, wintypes.WORD, wintypes.DWORD,
        ctypes.POINTER(wintypes.LPCWSTR), wintypes.LPVOID,
    )
    library.ReportEventW.restype = wintypes.BOOL
    library.DeregisterEventSource.argtypes = (wintypes.HANDLE,)
    library.DeregisterEventSource.restype = wintypes.BOOL

    handle = library.RegisterEventSourceW(None, SOURCE)
    if not handle:
        raise RuntimeError("Cannot open Event Log source")
    try:
        strings = (wintypes.LPCWSTR * 1)(message)
        if not library.ReportEventW(handle, event_type, 0, event_id, None, 1, 0, strings, None):
            raise RuntimeError("Event Log rejected the event")
    finally:
        library.DeregisterEventSource(handle)


def run_monitor(arguments, mode=()):
    command = [sys.executable, str(Path(__file__).with_name("reward_source_monitor.py")),
               "--psql", str(arguments.psql), "--database", arguments.database,
               "--state", str(arguments.state), "--timeout-seconds", str(arguments.timeout_seconds)]
    for world in sorted(set(arguments.world)):
        command.extend(("--world", str(world)))
    command.extend(mode)

    with tempfile.TemporaryFile() as output:
        result = subprocess.run(command, stdout=output, stderr=subprocess.DEVNULL,
                                timeout=arguments.timeout_seconds + 15, check=False)
        if output.tell() > MAX_ROUTER_OUTPUT:
            raise RuntimeError("Monitor output exceeded its limit")
        output.seek(0)
        document = json.load(output)
    if not isinstance(document, dict) or result.returncode not in (0, 2):
        raise RuntimeError("Monitor returned an invalid result")
    return result.returncode, document


def deliver_head(arguments, result, write=write_event, run=run_monitor):
    for _ in range(MAX_EVENTS_PER_RUN):
        events = result.get("events", [])
        if not isinstance(events, list) or len(events) > 1:
            raise RuntimeError("Invalid monitor outbox response")
        if not events:
            return
        event = events[0]
        if not isinstance(event, dict) or set(event) != {"id", "key", "severity"}:
            raise RuntimeError("Invalid monitor event")
        event_id = event["id"]
        key = event["key"]
        severity = event["severity"]
        allowed_keys = {f"{world}:{name}" for world in arguments.world for name in monitor.CHECKS}
        allowed_keys.add("monitor")
        if (not isinstance(event_id, str) or not re.fullmatch(r"[0-9a-f]{32}", event_id)
                or not isinstance(key, str) or key not in allowed_keys
                or not isinstance(severity, str) or severity not in EVENT_IDS):
            raise RuntimeError("Invalid monitor event fields")
        message = json.dumps({"scope": scope_id(arguments.database, arguments.world),
                              "id": event_id, "key": key, "severity": severity},
                             sort_keys=True, separators=(",", ":"))
        write(EVENT_IDS[severity], EVENT_TYPES[severity], message)
        code, result = run(arguments, ("--ack", event_id))
        if code != 0 or result.get("status") != "pending":
            raise RuntimeError("Monitor acknowledgement failed")
    if result.get("events"):
        raise RuntimeError("Monitor outbox exceeds the per-run delivery limit")


def sample_and_route(arguments, write=write_event, run=run_monitor):
    failure_message = json.dumps({"scope": scope_id(arguments.database, arguments.world),
                                  "status": "failed"}, sort_keys=True)
    try:
        code, result = run(arguments)
        if not isinstance(result, dict):
            raise RuntimeError("Monitor returned an invalid result")
        status = result.get("status")
        if code == 2 and status == "failed":
            write(FAILURE_EVENT, 1, failure_message)
            deliver_head(arguments, result, write, run)
            return 2
        if code != 0 or status != "sampled":
            raise RuntimeError("Monitor returned an invalid status")
        heartbeat = result.get("heartbeat")
        if type(heartbeat) is not int or heartbeat < 0:
            raise RuntimeError("Monitor returned an invalid heartbeat")

        # A successful sample is not a healthy router until its outbox is delivered.
        deliver_head(arguments, result, write, run)
        write(SUCCESS_EVENT, 4, json.dumps({"scope": scope_id(arguments.database, arguments.world),
                                           "status": "sampled"}, sort_keys=True))
        monitor.write_state(heartbeat_path(arguments.state),
                            {"scope": scope_id(arguments.database, arguments.world),
                             "last_success": heartbeat})
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired):
        # A failed destination may reject this too; the caller must also supervise our exit.
        try:
            write(FAILURE_EVENT, 1, failure_message)
        except (OSError, ValueError, RuntimeError):
            print("Reward monitor failure event unavailable", file=sys.stderr)
        raise


def heartbeat_path(state):
    return state.with_suffix(state.suffix + ".event-heartbeat")


def watchdog(arguments, now=None, write=write_event):
    now = time.time() if now is None else now
    try:
        heartbeat = heartbeat_path(arguments.state)
        if heartbeat.stat().st_size > MAX_ROUTER_OUTPUT:
            raise RuntimeError("Monitor state exceeded its limit")
        state = json.loads(heartbeat.read_text(encoding="utf-8"))
        if (not isinstance(state, dict) or set(state) != {"scope", "last_success"}
                or state["scope"] != scope_id(arguments.database, arguments.world)):
            raise RuntimeError("Monitor heartbeat scope does not match")
        last_success = state["last_success"]
        healthy = (type(last_success) is int and 0 <= now - last_success <= arguments.max_age_seconds)
    except (OSError, ValueError, RuntimeError):
        healthy = False
    if not healthy:
        write(STALE_EVENT, 1, json.dumps({"scope": scope_id(arguments.database, arguments.world),
                                         "status": "stale"}, sort_keys=True))
        return 2
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("sample", "watchdog"))
    parser.add_argument("--psql", type=Path, required=True)
    parser.add_argument("--database", required=True)
    parser.add_argument("--world", type=int, action="append", required=True)
    parser.add_argument("--state", type=Path, required=True)
    parser.add_argument("--timeout-seconds", type=int, default=15, choices=range(1, 61))
    parser.add_argument("--max-age-seconds", type=int, default=900, choices=range(60, 86401))
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_]{1,63}", args.database):
        parser.error("database must be a simple database name")
    if len(set(args.world)) > monitor.MAX_WORLDS or any(not 0 < world <= 2147483647 for world in args.world):
        parser.error("invalid world list")
    try:
        return sample_and_route(args) if args.mode == "sample" else watchdog(args)
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired):
        # Do not echo subprocess output, paths, credentials or raw Windows errors.
        print("Reward monitor Event Log routing failed", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
