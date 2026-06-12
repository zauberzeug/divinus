#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# ///
"""Hardware-in-the-loop test for HTTP stream slot exhaustion.

Hardware-gated (needs a reachable camera + SSH for fd accounting), so it is
NOT run in CI. Deploy a LIGHT config first (e.g. mjpeg 640x360@5, bitrate
~320) so 50 concurrent clients do not saturate the camera link.

Verifies that when all HTTP_MAX_CLIENTS (50) stream slots are taken:
  1. 50 /mjpeg clients register and stream (each gets a 200 + frames),
  2. the 51st /mjpeg request is refused with 503 and the socket is closed
     (EOF), instead of hanging with 200 headers and leaking the fd,
  3. another endpoint (/video.264) is refused the same way (shared helper),
  4. the 50 registered clients keep receiving data throughout,
  5. after all clients disconnect, the daemon's /proc/<pid>/fd count returns
     to baseline (no fd leak) and the PID is unchanged (no crash/respawn).

Usage: uv run test_slot_exhaustion.py [camera-ip]
"""

import socket
import subprocess
import sys
import threading
import time

CAM = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.94"
PASS = "Adminadmin"
SLOTS = 50

results: list[tuple[str, bool, str]] = []


def report(name: str, ok: bool, detail: str) -> None:
    results.append((name, ok, detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}: {detail}")


def cam_sh(cmd: str) -> str:
    out = subprocess.run(
        ["sshpass", "-p", PASS, "ssh", "-o", "StrictHostKeyChecking=no",
         "-o", "ConnectTimeout=8", f"root@{CAM}", cmd],
        capture_output=True, text=True, timeout=20)
    return out.stdout.strip()


def divinus_pid() -> str:
    return cam_sh("pidof divinus")


def fd_count(pid: str) -> int:
    return int(cam_sh(f"ls /proc/{pid}/fd | wc -l"))


def http_get(path: str, timeout: float = 10.0) -> tuple[socket.socket, str]:
    """Open a connection, send GET <path>, return (socket, status line)."""
    sock = socket.create_connection((CAM, 80), timeout=timeout)
    sock.sendall(f"GET {path} HTTP/1.1\r\nHost: {CAM}\r\n\r\n".encode())
    buf = b""
    while b"\r\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            break
        buf += chunk
    return sock, buf.split(b"\r\n", 1)[0].decode(errors="replace")


class Drainer(threading.Thread):
    """Keeps a registered stream client alive by consuming its data."""

    def __init__(self, sock: socket.socket):
        super().__init__(daemon=True)
        self.sock = sock
        self.received = 0
        self.closed = False
        self.start()

    def run(self):
        try:
            while True:
                chunk = self.sock.recv(65536)
                if not chunk:
                    break
                self.received += len(chunk)
        except OSError:
            pass
        self.closed = True


def read_to_eof(sock: socket.socket, timeout: float = 10.0) -> tuple[bool, bytes]:
    """Drain a socket until EOF; returns (saw_eof, body)."""
    sock.settimeout(timeout)
    body = b""
    try:
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                return True, body
            body += chunk
    except socket.timeout:
        return False, body


print(f"=== divinus stream slot exhaustion test against {CAM} ===")

pid_before = divinus_pid()
if not pid_before:
    print("FATAL: divinus is not running on the camera")
    sys.exit(1)
baseline_fds = fd_count(pid_before)
print(f"divinus pid {pid_before}, baseline fd count {baseline_fds}")

print(f"[1/5] registering {SLOTS} /mjpeg clients...")
drainers: list[Drainer] = []
bad = []
for n in range(SLOTS):
    sock, status = http_get("/mjpeg")
    if "200" not in status:
        bad.append(f"client {n + 1}: {status!r}")
        sock.close()
        continue
    drainers.append(Drainer(sock))
report("fill-50-slots", len(drainers) == SLOTS,
       f"{len(drainers)}/{SLOTS} clients streaming" +
       (f"; refused: {'; '.join(bad[:3])}" if bad else ""))

print("[2/5] 51st /mjpeg client must get 503 and EOF...")
sock51, status51 = http_get("/mjpeg")
eof51, _ = read_to_eof(sock51)
sock51.close()
report("51st-mjpeg-503", "503" in status51 and eof51,
       f"status {status51!r}, EOF={'yes' if eof51 else 'NO (socket left hanging)'}")

print("[3/5] /video.264 must be refused the same way...")
sock52, status52 = http_get("/video.264")
eof52, _ = read_to_eof(sock52)
sock52.close()
report("h264-503", "503" in status52 and eof52,
       f"status {status52!r}, EOF={'yes' if eof52 else 'NO (socket left hanging)'}")

print("[4/5] registered clients must keep streaming...")
marks = [d.received for d in drainers]
time.sleep(3)
stalled = sum(1 for d, m in zip(drainers, marks) if d.received <= m or d.closed)
report("50-clients-still-streaming", stalled == 0,
       f"{len(drainers) - stalled}/{len(drainers)} advanced after refusals")

print("[5/5] fd count must return to baseline after disconnect...")
for d in drainers:
    d.sock.close()
fds_after = -1
deadline = time.monotonic() + 30
while time.monotonic() < deadline:
    fds_after = fd_count(pid_before)
    if fds_after <= baseline_fds:
        break
    time.sleep(2)
report("no-fd-leak", fds_after <= baseline_fds,
       f"baseline {baseline_fds}, after {fds_after}")

pid_after = divinus_pid()
report("no-crash", pid_after == pid_before, f"pid {pid_before} -> {pid_after}")

print()
failed = [r for r in results if not r[1]]
print(f"=== {len(results) - len(failed)}/{len(results)} passed ===")
sys.exit(1 if failed else 0)
