#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow"]
# ///
"""Official hardware-in-the-loop stream-integrity suite for divinus.

Hardware-gated (needs a reachable camera), so it is NOT run in CI — run it by
hand before shipping any stream-path change.

Verifies against a live camera that
  1. MJPEG frames arrive complete (no truncated/torn JPEGs) for a fast consumer
     over a SUSTAINED run (>= 30 s: the historical fast-client kick fired at
     ~10 s, after a short check had already passed),
  2. a SLOW MJPEG consumer is NOT disconnected (policy 2026-06-12: backpressure
     skips frames, it never terminates a live client) and receives only
     complete JPEGs — fewer frames per second is fine and expected,
  3. RTSP delivers decodable H26x video over TCP and UDP transport (ffmpeg),
  4. the fMP4 HTTP stream decodes (bonus, same writev path as /video.26x).

Usage: uv run test_streams.py [camera-ip]
"""

import io
import socket
import subprocess
import sys
import time

from PIL import Image, ImageFile

ImageFile.LOAD_TRUNCATED_IMAGES = False  # truncated JPEG must raise, that's the point

CAM = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.223"
BOUNDARY = b"--boundarydonotcross"

results: list[tuple[str, bool, str]] = []


def report(name: str, ok: bool, detail: str) -> None:
    results.append((name, ok, detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}: {detail}")


def read_mjpeg_frames(duration_s: float, recv_size: int, delay_s: float):
    """Yield (content_length, jpeg_bytes) parts from /mjpeg for duration_s;
    a small recv_size plus delay throttles the drain rate to simulate a slow
    consumer building steady TCP backpressure."""
    sock = socket.create_connection((CAM, 80), timeout=10)
    sock.sendall(f"GET /mjpeg HTTP/1.1\r\nHost: {CAM}\r\nConnection: keep-alive\r\n\r\n".encode())
    buf = b""
    deadline = time.monotonic() + duration_s
    got = 0
    try:
        while time.monotonic() < deadline:
            try:
                chunk = sock.recv(recv_size)
            except socket.timeout:
                raise ConnectionError(f"recv timeout (no data for 10 s) after {got} frames")
            if not chunk:
                raise ConnectionError(f"server closed connection after {got} frames")
            buf += chunk
            if delay_s:
                time.sleep(delay_s)
            while True:
                start = buf.find(BOUNDARY)
                if start < 0:
                    break
                head_end = buf.find(b"\r\n\r\n", start)
                if head_end < 0:
                    break
                headers = buf[start:head_end].decode(errors="replace")
                clen = None
                for line in headers.split("\r\n"):
                    if line.lower().startswith("content-length:"):
                        clen = int(line.split(":", 1)[1].strip())
                body_start = head_end + 4
                if clen is None or len(buf) < body_start + clen + 2:
                    break
                body = buf[body_start : body_start + clen]
                buf = buf[body_start + clen :]
                got += 1
                yield clen, body
    finally:
        sock.close()


def check_jpeg(body: bytes, clen: int) -> str | None:
    """Return an error description if the JPEG is torn/invalid, else None."""
    if len(body) != clen:
        return f"got {len(body)} bytes, Content-Length said {clen}"
    if not body.startswith(b"\xff\xd8"):
        return "missing SOI marker (frame desync)"
    if b"\xff\xd9" not in body[-4:]:
        return "missing EOI marker (truncated frame)"
    try:
        img = Image.open(io.BytesIO(body))
        img.load()  # raises on truncated scan data
    except Exception as e:
        return f"decode failed: {e}"
    return None


def test_mjpeg(name: str, duration_s: float, recv_size: int, delay_s: float,
               min_frames: int) -> None:
    """The server must never disconnect a live MJPEG client — a slow one gets
    fewer frames (frame-granular skipping), not a termination — and every
    delivered JPEG must be complete."""
    torn, sizes = [], []
    disconnected = ""
    t0 = time.monotonic()
    try:
        for clen, body in read_mjpeg_frames(duration_s, recv_size, delay_s):
            sizes.append(clen)
            err = check_jpeg(body, clen)
            if err:
                torn.append(f"frame {len(sizes)}: {err}")
    except (ConnectionError, TimeoutError) as e:
        disconnected = f"disconnected after {time.monotonic() - t0:.1f}s: {e}"
    n = len(sizes)
    elapsed = max(time.monotonic() - t0, 0.001)
    detail = f"{n} frames in {elapsed:.1f}s ({n / elapsed:.1f} fps), avg {sum(sizes) // max(n, 1)} B"
    if disconnected:
        report(name, False, f"{detail}; {disconnected}")
    elif torn:
        report(name, False, f"{detail}; TORN: {'; '.join(torn[:3])}")
    elif n < min_frames:
        report(name, False, f"{detail}; expected >= {min_frames} frames")
    else:
        report(name, True, detail)


def test_rtsp(name: str, transport: str, seconds: int = 8) -> None:
    cmd = [
        "ffmpeg", "-hide_banner", "-loglevel", "info",  # info: progress lines carry the frame count
        "-rtsp_transport", transport, "-i", f"rtsp://{CAM}/stream0",
        "-t", str(seconds), "-f", "null", "-",
    ]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=seconds + 25)
    except subprocess.TimeoutExpired:
        report(name, False, "ffmpeg hung (no data?)")
        return
    frames = 0
    for line in proc.stderr.replace("\r", "\n").splitlines():
        if line.strip().startswith("frame="):
            frames = int(line.split("frame=")[1].split()[0])
    # decode-relevant complaints only; skip progress/metadata noise
    errs = [
        l for l in proc.stderr.splitlines()
        if "frame=" not in l
        and any(s in l.lower() for s in ("corrupt", "invalid", "missing picture", "concealing", "error"))
    ]
    ok = proc.returncode == 0 and frames >= 10 and not errs
    detail = f"{frames} frames decoded over {transport.upper()}"
    if errs:
        detail += f"; {len(errs)} decode errors, first: {errs[0].strip()[:90]}"
    if proc.returncode != 0:
        detail += f"; ffmpeg rc={proc.returncode}: {proc.stderr.strip()[-120:]}"
    report(name, ok, detail)


def test_fmp4(seconds: int = 6) -> None:
    cmd = [
        "ffmpeg", "-hide_banner", "-loglevel", "warning",
        "-i", f"http://{CAM}/video.mp4", "-t", str(seconds), "-f", "null", "-",
    ]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=seconds + 20)
    except subprocess.TimeoutExpired:
        report("fmp4-http", False, "ffmpeg hung")
        return
    errs = [l for l in proc.stderr.splitlines() if "error" in l.lower() or "corrupt" in l.lower()]
    report("fmp4-http", proc.returncode == 0 and not errs,
           f"rc={proc.returncode}, {len(errs)} errors" + (f", first: {errs[0].strip()[:90]}" if errs else ""))


only = set(sys.argv[2:])  # optional test names to run, e.g. mjpeg-fast rtsp-tcp


def wanted(name: str) -> bool:
    return not only or name in only


print(f"=== divinus stream integrity tests against {CAM} ===")

if wanted("mjpeg-fast"):
    print("[1/5] MJPEG, fast consumer (64 KiB reads, no delay, 30 s sustained)...")
    test_mjpeg("mjpeg-fast", duration_s=30.0, recv_size=65536, delay_s=0, min_frames=200)

if wanted("mjpeg-slow"):
    print("[2/5] MJPEG, slow consumer (8 KiB reads, 10 ms delay -> steady ~0.8 MB/s drain)...")
    test_mjpeg("mjpeg-slow", duration_s=20.0, recv_size=8192, delay_s=0.01, min_frames=5)

if wanted("rtsp-tcp"):
    print("[3/5] RTSP over TCP...")
    test_rtsp("rtsp-tcp", "tcp")

if wanted("rtsp-udp"):
    print("[4/5] RTSP over UDP...")
    test_rtsp("rtsp-udp", "udp")

if wanted("fmp4-http"):
    print("[5/5] fMP4 over HTTP...")
    test_fmp4()

print()
failed = [r for r in results if not r[1]]
print(f"=== {len(results) - len(failed)}/{len(results)} passed ===")
sys.exit(1 if failed else 0)
