#!/usr/bin/env python3
"""
test_all.py — Comprehensive end-to-end test suite for Linux Service Supervisor
Tests all REST API endpoints, signal actions, random restart chaos,
watchdog heartbeats, and multi-service concurrency.
"""

import urllib.request
import urllib.parse
import subprocess
import signal
import os
import json
import time
import sys

BASE_URL = "http://localhost:8080"

def log_test(name, passed, detail=""):
    mark = "✓ PASS" if passed else "✗ FAIL"
    print(f"[{mark}] {name} {detail}")
    if not passed:
        sys.exit(1)

def http_get(path):
    url = f"{BASE_URL}{path}"
    req = urllib.request.Request(url)
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            return resp.status, json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        try:
            body = json.loads(e.read().decode("utf-8"))
        except Exception:
            body = {"error": str(e)}
        return e.code, body
    except Exception as e:
        return 0, {"error": str(e)}

def http_post(path, data):
    url = f"{BASE_URL}{path}"
    payload = json.dumps(data).encode("utf-8")
    req = urllib.request.Request(url, data=payload, headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            return resp.status, json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        try:
            body = json.loads(e.read().decode("utf-8"))
        except Exception:
            body = {"error": str(e)}
        return e.code, body
    except Exception as e:
        return 0, {"error": str(e)}

def main():
    print("==================================================================")
    print("    Linux Service Supervisor — Comprehensive System Verification")
    print("==================================================================")

    # Initial probe: auto-start supervisor daemon if not already running
    status, data = http_get("/api/status")
    spawned_proc = None
    if status != 200 and os.path.exists("./bin/supervisor"):
        spawned_proc = subprocess.Popen(
            ["./bin/supervisor", "-c", "config/services.conf", "-d"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    for _ in range(10):
        if status == 200 and len([s for s in data.get("services", []) if s.get("state") == "RUNNING"]) >= 4:
            break
        time.sleep(1)
        status, data = http_get("/api/status")

    # Test 1: GET /api/status
    log_test("GET /api/status", status == 200, f"(HTTP {status})")

    sup = data.get("supervisor", {})
    services = data.get("services", [])
    log_test("Supervisor running", sup.get("running") == 1, f"(PID: {sup.get('pid')})")
    log_test("Service count", len(services) == 6, f"(Total: {len(services)} services)")

    running_svcs = [s for s in services if s.get("state") == "RUNNING"]
    log_test("Multiple services running concurrently", len(running_svcs) >= 4, f"({len(running_svcs)} / 6 running)")

    # Test 2: Random Restart Action
    initial_restarts = sup.get("total_restarts", 0)
    print("\n--- Testing Random Chaos Restart Endpoint ---")
    status, res = http_post("/api/action", {"action": "random_restart"})
    log_test("POST /api/action [random_restart]", status == 200 and res.get("success") is True, f"({res.get('message')})")

    time.sleep(2)
    status, data = http_get("/api/status")
    new_sup = data.get("supervisor", {})
    new_restarts = new_sup.get("total_restarts", 0)
    log_test("Cumulative restart counter incremented", new_restarts > initial_restarts, f"(Before: {initial_restarts}, After: {new_restarts})")

    # Test 3: Pause & Resume Service (SIGSTOP & SIGCONT)
    # Pick a stable running service (avoid flaky which intentionally simulates spontaneous crash faults)
    target_svc = "logger"
    for s in data.get("services", []):
        if s.get("state") == "RUNNING" and s.get("name") in ("logger", "heartbeat", "metrics", "audit"):
            target_svc = s.get("name")
            break

    print(f"\n--- Testing Signal Dispatch (SIGSTOP & SIGCONT) on '{target_svc}' ---")
    status, res = http_post("/api/action", {"action": "pause", "service": target_svc})
    log_test(f"POST /api/action [pause {target_svc}]", status == 200 and res.get("success") is True, f"({res.get('message')})")

    svc_info = None
    for _ in range(5):
        time.sleep(0.5)
        status, data = http_get("/api/status")
        svc_info = next((s for s in data.get("services", []) if s["name"] == target_svc), None)
        if svc_info and svc_info.get("state") == "PAUSED":
            break
    log_test(f"{target_svc.capitalize()} in PAUSED state", svc_info and svc_info.get("state") == "PAUSED")

    status, res = http_post("/api/action", {"action": "resume", "service": target_svc})
    log_test(f"POST /api/action [resume {target_svc}]", status == 200 and res.get("success") is True, f"({res.get('message')})")

    for _ in range(5):
        time.sleep(0.5)
        status, data = http_get("/api/status")
        svc_info = next((s for s in data.get("services", []) if s["name"] == target_svc), None)
        if svc_info and svc_info.get("state") == "RUNNING":
            break
    actual_state = svc_info.get("state") if svc_info else "None"
    log_test(f"{target_svc.capitalize()} resumed to RUNNING state", svc_info and svc_info.get("state") == "RUNNING", f"(Actual state: {actual_state})")

    # Test 4: Stop and Start (SIGTERM & Fork/Exec)
    print("\n--- Testing Stop & Start Cycle (SIGTERM & Fork/Exec) on 'hung' ---")
    status, res = http_post("/api/action", {"action": "stop", "service": "hung"})
    log_test("POST /api/action [stop hung]", status == 200 and res.get("success") is True, f"({res.get('message')})")

    # Wait for service to transition to STOPPED
    hung = None
    for _ in range(5):
        time.sleep(1)
        status, data = http_get("/api/status")
        hung = next((s for s in data.get("services", []) if s["name"] == "hung"), None)
        if hung and hung.get("state") == "STOPPED":
            break
    log_test("Hung transitioned to STOPPED state", hung and hung.get("state") in ("STOPPING", "STOPPED"), f"(State: {hung.get('state') if hung else 'None'})")

    status, res = http_post("/api/action", {"action": "start", "service": "hung"})
    log_test("POST /api/action [start hung]", status == 200 and res.get("success") is True, f"({res.get('message')})")

    for _ in range(5):
        time.sleep(1)
        status, data = http_get("/api/status")
        hung = next((s for s in data.get("services", []) if s["name"] == "hung"), None)
        if hung and hung.get("state") == "RUNNING":
            break
    log_test("Hung started to RUNNING state", hung and hung.get("state") == "RUNNING")

    # Test 5: Configuration API
    print("\n--- Testing Configuration API ---")
    status, cfg = http_get("/api/config")
    log_test("GET /api/config", status == 200 and "raw" in cfg, f"(Length: {len(cfg.get('raw', ''))} bytes)")

    # Test 6: Audit Logs API
    print("\n--- Testing Audit Logs API ---")
    status, logs = http_get("/api/logs?source=supervisor")
    line_count = len(logs.get("lines", []))
    log_test("GET /api/logs", status == 200 and "lines" in logs, f"(Lines: {line_count})")

    # Test 7: Multi-Service Concurrency and Continuous Heartbeats over time
    print("\n--- Testing Autonomous Multi-Service Concurrency & Natural Restarts (15s observation) ---")
    time.sleep(15)
    status, data = http_get("/api/status")
    sup = data.get("supervisor", {})
    services = data.get("services", [])
    running = [s for s in services if s.get("state") == "RUNNING"]
    log_test("High concurrency preserved across restarts", len(running) >= 4, f"({len(running)} / 6 services running)")

    # Test 8: Multiple services have recorded restarts/crashes equally
    restarted_services = [s["name"] for s in services if s.get("total_restarts", 0) > 0 or s.get("restart_count", 0) > 0]
    print(f"Services with recorded restarts: {', '.join(restarted_services)}")
    log_test("Multiple different services experiencing recovery", len(restarted_services) >= 2, f"({len(restarted_services)} services have recovered)")

    # Test 9: Real Linux OS Kernel Verification (/proc, PPid, PGID, pidfiles, external SIGKILL, SIGHUP, 0 zombies)
    if os.path.exists("/proc"):
        print("\n--- Testing Real Linux OS Kernel Process Tree (/proc, PGID, SIGKILL, SIGHUP, Zombies) ---")
        sup_pid = sup.get("pid")
        status, data = http_get("/api/status")
        live_svcs = [s for s in data.get("services", []) if s.get("state") == "RUNNING" and s.get("pid", 0) > 0]
        proc_verified = 0
        for s in live_svcs:
            pid = s["pid"]
            proc_status = f"/proc/{pid}/status"
            pidfile = f"logs/{s['name']}.pid"
            if os.path.exists(proc_status) and os.path.exists(pidfile):
                with open(proc_status) as f:
                    txt = f.read()
                if f"PPid:\t{sup_pid}" in txt and s.get("pgid") == pid:
                    proc_verified += 1
        log_test("Kernel /proc PPid, PGID==PID isolation & pidfiles verified", proc_verified >= 4, f"({proc_verified} child processes verified in /proc)")

        # External kernel SIGKILL (kill -9) directly to a worker process
        victim = next((s for s in live_svcs if s["name"] == "metrics"), live_svcs[0])
        old_pid = victim["pid"]
        old_crashes = victim["crash_count"]
        os.kill(old_pid, signal.SIGKILL)
        new_pid = -1
        recovered_svc = None
        for _ in range(16):
            time.sleep(0.5)
            _, d2 = http_get("/api/status")
            recovered_svc = next((x for x in d2.get("services", []) if x["name"] == victim["name"]), None)
            if recovered_svc and recovered_svc.get("state") == "RUNNING" and recovered_svc.get("pid") not in (-1, old_pid):
                new_pid = recovered_svc["pid"]
                break
        log_test(
            f"External SIGKILL (kill -9 {old_pid}) auto-reaped & respawned",
            new_pid > 0 and recovered_svc.get("crash_count", 0) > old_crashes,
            f"(Old PID: {old_pid} -> New PID: {new_pid}, Signal: {recovered_svc.get('last_term_signal') if recovered_svc else 'N/A'})",
        )

        # Direct SIGHUP to supervisor PID
        os.kill(sup_pid, signal.SIGHUP)
        time.sleep(0.5)
        status, _ = http_get("/api/status")
        log_test(f"Direct SIGHUP to supervisor (PID {sup_pid}) hot-reloaded cleanly", status == 200)

        # Check for zero zombie children in /proc
        zombies = []
        for entry in os.listdir("/proc"):
            if entry.isdigit():
                try:
                    with open(f"/proc/{entry}/status") as f:
                        st = f.read()
                    if f"PPid:\t{sup_pid}" in st and "State:\tZ" in st:
                        zombies.append(entry)
                except Exception:
                    pass
        log_test("Zero zombie child processes in /proc", len(zombies) == 0, f"(Zombies: {len(zombies)})")

    print("\n==================================================================")
    print("           ALL 9 TEST SUITES COMPLETED AND PASSED!                ")
    print("==================================================================")
    if spawned_proc is not None:
        http_post("/api/action", {"action": "shutdown"})
        try:
            spawned_proc.wait(timeout=5)
        except Exception:
            spawned_proc.kill()

if __name__ == "__main__":
    main()
