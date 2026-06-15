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
  4. the fMP4 HTTP stream decodes (bonus, same writev path as /video.26x),
  5. the raw UDP push enables/disables at runtime via /api/stream with no reboot,
     emitting decodable RTP (split SPS/PPS/IDR) to this host while enabled.

Usage: uv run test_streams.py [camera-ip]
"""

import io
import re
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


def api(path: str, timeout: float = 8.0) -> str:
    """GET an /api/* endpoint and return the response body."""
    s = socket.create_connection((CAM, 80), timeout=timeout)
    s.sendall(f"GET {path} HTTP/1.1\r\nHost: {CAM}\r\nConnection: close\r\n\r\n".encode())
    s.settimeout(timeout)
    data = b""
    while True:
        c = s.recv(4096)
        if not c:
            break
        data += c
    s.close()
    return data.split(b"\r\n\r\n", 1)[-1].decode(errors="replace")


def api_int(path: str, key: str) -> int:
    m = re.search(rf'"{key}":(-?\d+)', api(path))  # -1 = absent/unreadable
    return int(m.group(1)) if m else -1


def api_bool(path: str, key: str) -> bool:
    return re.search(rf'"{key}":true', api(path)) is not None


def grab_luma() -> float:
    """Mean luma (0-255) of one fresh MJPEG frame."""
    for _clen, body in read_mjpeg_frames(duration_s=6.0, recv_size=65536, delay_s=0):
        h = Image.open(io.BytesIO(body)).convert("L").histogram()
        n = sum(h)
        return sum(i * h[i] for i in range(256)) / n if n else -1.0
    return -1.0


def test_exposure_reacts_to_fps() -> None:
    """A live fps change (web-UI 'Apply' path, no restart) must move the sensor
    rate, so `exposure: max` re-pins to the new frame budget. Regression: the
    sensor rate used to update only at boot, so max stayed at the boot budget."""
    name = "exposure-fps-react"
    mp4_was = api_bool("/api/mp4", "enable")
    mjpeg_fps_was = api_int("/api/mjpeg", "fps")
    try:
        api("/api/mp4?enable=false")  # mjpeg alone drives the sensor rate
        api("/api/mjpeg?enable=true&fps=20"); time.sleep(2)
        api("/api/exposure?value=max"); time.sleep(2)
        hi = api_int("/api/gain", "shutter_us")          # ~ 1e6/20 = 50000
        api("/api/mjpeg?fps=5"); time.sleep(2)            # live Apply, no restart
        api("/api/exposure?value=max"); time.sleep(2)
        lo = api_int("/api/gain", "shutter_us")          # must react -> ~ 1e6/5 = 200000
        ok = hi > 0 and lo > hi * 2
        report(name, ok, f"max shutter @20fps={hi}us -> @5fps={lo}us "
                         f"(expect ~50000 -> ~200000; no restart)")
    finally:
        if mp4_was:
            api("/api/mp4?enable=true")
        if mjpeg_fps_was > 0:
            api(f"/api/mjpeg?fps={mjpeg_fps_was}")
        api("/api/exposure?value=0")


def test_exposure_brightness() -> None:
    """With AE gain pinned to 1x, image brightness must track the shutter — a
    longer fixed exposure is visibly brighter. Guards against trusting the
    shutter read-back when the actual integration does not change."""
    name = "exposure-brightness"
    try:
        api("/api/gain?min_gain=1024&max_gain=1024&min_isp_gain=1024&max_isp_gain=1024")
        api("/api/exposure?value=2000"); time.sleep(2)
        dim = grab_luma()
        api("/api/exposure?value=20000"); time.sleep(2)
        bright = grab_luma()
        ok = dim >= 0 and bright > dim + 5
        report(name, ok, f"luma @2ms={dim:.1f} -> @20ms={bright:.1f} "
                         f"(gain pinned 1x; longer shutter must brighten)")
    finally:
        api("/api/gain?min_gain=0&max_gain=0&min_isp_gain=0&max_isp_gain=0")
        api("/api/exposure?value=0")


def host_ip_seen_by_camera() -> str:
    """The local address the camera reaches us on — the UDP push destination."""
    s = socket.create_connection((CAM, 80), timeout=8)
    ip = s.getsockname()[0]
    s.close()
    return ip


# NAL type tables (mirrors utils/verify_udp_stream.py): param sets + IDR per codec.
_H265 = {33, 34}, {19, 20}, 49  # (SPS,PPS), (IDR...), FU type
_H264 = {7, 8}, {5}, 28


def _seen_nal_types(pkts: list[bytes], h265: bool) -> set[int]:
    """Classify each RTP packet's NAL type, unwrapping FU fragments to the
    original type — so a split SPS/PPS/IDR shows up whether sent whole or
    fragmented."""
    _, _, fu = _H265 if h265 else _H264
    seen = set()
    for data in pkts:
        if len(data) < 14:
            continue
        p = data[12:]  # strip RTP header
        t = (p[0] >> 1) & 0x3F if h265 else p[0] & 0x1F
        if t == fu:
            fu_h = p[2] if h265 else p[1]
            seen.add(fu_h & 0x3F if h265 else fu_h & 0x1F)
        else:
            seen.add(t)
    return seen


def capture_rtp(port: int, secs: float) -> list[bytes]:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", port))
    sock.settimeout(1.0)
    out, deadline = [], time.monotonic() + secs
    try:
        while time.monotonic() < deadline:
            try:
                out.append(sock.recvfrom(65535)[0])
            except socket.timeout:
                continue
    finally:
        sock.close()
    return out


def test_stream_udp_runtime(port: int = 5600) -> None:
    """The raw UDP push must enable/disable at runtime via /api/stream with no
    reboot: enabling starts decodable RTP (split SPS/PPS/IDR) to our IP, disabling
    stops it, and GET reflects the live state. Needs the host to accept inbound
    UDP on `port`."""
    name = "stream-udp-runtime"
    host_ip = host_ip_seen_by_camera()
    dest = f"udp://{host_ip}:{port}"
    enable_was = api_bool("/api/stream", "enable")
    try:
        api(f"/api/stream?enable=true&dest={dest}")
        live_ok = api_bool("/api/stream", "enable") and dest in api("/api/stream")
        on = capture_rtp(port, 6.0)
        # Detect codec from the wire: H.265 if its SPS+PPS types show up.
        h265 = {33, 34}.issubset(_seen_nal_types(on, h265=True))
        sps_pps, idr, _ = _H265 if h265 else _H264
        seen = _seen_nal_types(on, h265)
        split_ok = sps_pps.issubset(seen) and bool(idr & seen)

        api("/api/stream?enable=false")
        time.sleep(1.0)  # let the last in-flight frame drain; the push then goes silent
        off = capture_rtp(port, 2.0)
        disabled_ok = api_bool("/api/stream", "enable") is False

        ok = live_ok and len(on) > 10 and split_ok and len(off) == 0 and disabled_ok
        report(name, ok,
                f"enabled: {len(on)} pkts, SPS/PPS/IDR split={split_ok}; "
                f"GET live={live_ok}; disabled: {len(off)} pkts, GET off={disabled_ok}")
    finally:
        if enable_was:
            api(f"/api/stream?enable=true&dest={dest}")
        else:
            api("/api/stream?enable=false")


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

if wanted("exposure-fps-react"):
    print("[6/7] Exposure budget reacts to a live fps change (no restart)...")
    test_exposure_reacts_to_fps()

if wanted("exposure-brightness"):
    print("[7/8] Exposure brightness tracks shutter (gain pinned 1x)...")
    test_exposure_brightness()

if wanted("stream-udp-runtime"):
    print("[8/8] UDP push enables/disables at runtime via /api/stream (no restart)...")
    test_stream_udp_runtime()

print()
failed = [r for r in results if not r[1]]
print(f"=== {len(results) - len(failed)}/{len(results)} passed ===")
sys.exit(1 if failed else 0)
