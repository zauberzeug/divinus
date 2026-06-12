#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy", "pillow"]
# ///
"""Measure divinus stream latency and frame cadence against a live camera.

    uv run measure_latency.py [ip] [--stream mjpeg|rtsp|fmp4|all]
                              [--stall mjpeg|fmp4] [--json-out PATH]

Two metrics:
- OSD inject-to-receive latency: toggle an OSD region via the HTTP API and
  detect the brightness change in decoded frames. Covers OSD->VENC->network->
  decode (not sensor/ISP). Needs `osd.enable: true`; the OSD applies on a 1 Hz
  poll, so the MIN over samples is the comparable number. The tool never edits
  the camera config and restores the OSD slot afterwards.
- Frame cadence: inter-frame arrival deltas (mean/stdev/p95/max, effective fps).
  Pair with --stall to run a slow competing consumer and expose send-path stalls.
"""

import argparse
import io
import json
import os
import select
import socket
import subprocess
import sys
import time
import urllib.error
import threading
import urllib.parse
import urllib.request
from collections.abc import Generator
from datetime import datetime

import numpy as np
from PIL import Image

BOUNDARY = b"--boundarydonotcross"
FF_W, FF_H = 320, 240          # ffmpeg rawvideo output size (aspect-distortion is fine)
WIN_X, WIN_Y = 0.45, 0.12      # normalized top-left detection window (covers the OSD block)
OSD_TEXT = "@" * 16            # dense glyphs -> large bright/dark block
OSD_SIZE = 120
OSD_POS = "16,16"
MIN_CONTRAST = 8.0             # gray levels the toggle must move the window mean
SAMPLE_TIMEOUT_S = 5.0         # 1 s OSD poll + pipeline + margin
STREAMS = ("mjpeg", "rtsp", "fmp4")


class MeasurementError(Exception):
    pass


# ---------------------------------------------------------------- HTTP helpers

def http_get(ip: str, path: str, timeout: float = 5.0) -> tuple[int, bytes]:
    try:
        with urllib.request.urlopen(f"http://{ip}{path}", timeout=timeout) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def probe_json(ip: str, path: str) -> dict | None:
    try:
        code, body = http_get(ip, path)
        return json.loads(body) if code == 200 else None
    except (OSError, ValueError):
        return None


# ---------------------------------------------------------------- frame sources

def mjpeg_frames(ip: str, max_seconds: float = 600.0, read_timeout: float = 10.0):
    """Yield (t_arrival_monotonic, jpeg_bytes) from /mjpeg (parser style of
    utils/test_streams.py)."""
    sock = socket.create_connection((ip, 80), timeout=10)
    sock.settimeout(read_timeout)
    sock.sendall(
        f"GET /mjpeg HTTP/1.1\r\nHost: {ip}\r\nConnection: keep-alive\r\n\r\n".encode())
    buf = b""
    deadline = time.monotonic() + max_seconds
    try:
        while time.monotonic() < deadline:
            try:
                chunk = sock.recv(65536)
            except socket.timeout:
                raise TimeoutError(f"MJPEG stream stalled ({read_timeout:.0f} s without data)")
            if not chunk:
                raise ConnectionError("server closed MJPEG connection")
            t = time.monotonic()
            buf += chunk
            while True:
                start = buf.find(BOUNDARY)
                if start < 0:
                    break
                head_end = buf.find(b"\r\n\r\n", start)
                if head_end < 0:
                    break
                clen = None
                for line in buf[start:head_end].decode(errors="replace").split("\r\n"):
                    if line.lower().startswith("content-length:"):
                        clen = int(line.split(":", 1)[1].strip())
                body_start = head_end + 4
                if clen is None or len(buf) < body_start + clen + 2:
                    break
                body = buf[body_start: body_start + clen]
                buf = buf[body_start + clen:]
                yield t, body
    finally:
        sock.close()


def ffmpeg_frames(url: str, transport: str | None = None, read_timeout: float = 15.0):
    """Yield (t_arrival_monotonic, HxW uint8 gray array) decoded by ffmpeg.

    -threads 1 keeps the decoder from adding frame-threading pipeline delay,
    nobuffer/low_delay minimize client-side buffering.
    """
    cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-nostats",
           "-fflags", "nobuffer", "-flags", "low_delay", "-threads", "1"]
    if transport:
        cmd += ["-rtsp_transport", transport]
    cmd += ["-i", url, "-an", "-vsync", "passthrough",
            "-vf", f"scale={FF_W}:{FF_H}",
            "-f", "rawvideo", "-pix_fmt", "gray", "pipe:1"]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, bufsize=0)
    assert proc.stdout is not None and proc.stderr is not None
    fd = proc.stdout.fileno()
    need = FF_W * FF_H
    buf = bytearray()
    try:
        while True:
            ready, _, _ = select.select([fd], [], [], read_timeout)
            if not ready:
                raise TimeoutError(f"stream stalled ({read_timeout:.0f} s without data)")
            chunk = os.read(fd, 1 << 16)
            t = time.monotonic()
            if not chunk:
                err = b""
                try:
                    proc.kill()
                    err = proc.stderr.read() or b""
                except OSError:
                    pass
                raise ConnectionError(
                    f"ffmpeg exited: {err.decode(errors='replace').strip()[-200:]}")
            buf += chunk
            while len(buf) >= need:
                frame = np.frombuffer(bytes(buf[:need]), np.uint8).reshape(FF_H, FF_W)
                del buf[:need]
                yield t, frame
    finally:
        proc.kill()
        proc.wait()


def make_source(stream: str, ip: str, max_seconds: float = 600.0, read_timeout: float = 10.0):
    if stream == "mjpeg":
        return mjpeg_frames(ip, max_seconds, read_timeout)
    if stream == "rtsp":
        return ffmpeg_frames(f"rtsp://{ip}/stream0", transport="tcp", read_timeout=read_timeout)
    if stream == "fmp4":
        return ffmpeg_frames(f"http://{ip}/video.mp4", read_timeout=read_timeout)
    raise ValueError(stream)


def jpeg_gray(jpeg: bytes) -> np.ndarray:
    img = Image.open(io.BytesIO(jpeg))
    img.draft("L", (max(1, img.width // 8), max(1, img.height // 8)))  # fast DCT-scaled decode
    return np.asarray(img.convert("L"), np.uint8)


def gray_frames(stream: str, ip: str) -> Generator[tuple[float, np.ndarray], None, None]:
    for t, item in make_source(stream, ip):
        yield t, jpeg_gray(item) if isinstance(item, bytes) else item


def window_mean(gray: np.ndarray) -> float:
    h, w = gray.shape
    return float(gray[: max(1, int(h * WIN_Y)), : max(1, int(w * WIN_X))].mean())


# ---------------------------------------------------------------- OSD control

class OsdController:
    """Drives /api/osd/<id>; saves original state on probe, restores on exit."""

    def __init__(self, ip: str, osd_id: int):
        self.ip = ip
        self.base = f"/api/osd/{osd_id}"
        self.orig: dict | None = None
        self.dirty = False

    def probe(self) -> tuple[bool, str]:
        try:
            code, body = http_get(self.ip, self.base)
        except OSError as e:
            return False, f"request failed: {e}"
        text = body.decode(errors="replace").strip()
        if code != 200:
            return False, (f"HTTP {code}: {text[:100]} -- the OSD API is disabled; "
                           "end-to-end latency needs `osd: enable: true` in "
                           "/etc/divinus.yaml (+ divinus restart, not done by this tool)")
        try:
            self.orig = json.loads(text)
        except ValueError:
            self.orig = None
        if self.orig and not self.orig.get("text") and self.orig.get("img"):
            return False, (f"OSD slot holds an image region ({self.orig['img']}) which "
                           "cannot be restored through the API; pick another --osd-id")
        return True, "ok"

    def _set(self, **params) -> float:
        """Apply params; return the toggle instant (midpoint of the request)."""
        query = "&".join(f"{k}={urllib.parse.quote(str(v), safe='')}"
                         for k, v in params.items())
        t0 = time.monotonic()
        code, _ = http_get(self.ip, f"{self.base}?{query}")
        t1 = time.monotonic()
        if code != 200:
            raise MeasurementError(f"OSD set failed: HTTP {code}")
        self.dirty = True
        return (t0 + t1) / 2

    def apply(self, on: bool) -> float:
        color = "#FFF" if on else "#000"
        return self._set(text=OSD_TEXT, size=OSD_SIZE, pos=OSD_POS,
                         opal=255, color=color, outl=color, thick=0)

    def restore(self) -> None:
        if not self.dirty:
            return
        o = self.orig or {}
        if o.get("text"):
            params = {"text": o["text"], "size": o.get("size", 32),
                      "color": o.get("color", "#FFF"), "opal": o.get("opal", 255),
                      "outl": o.get("outl", "#8000"), "thick": o.get("thick", 0)}
            if isinstance(o.get("pos"), list) and len(o["pos"]) == 2:
                params["pos"] = f"{o['pos'][0]},{o['pos'][1]}"
            if o.get("font"):
                params["font"] = o["font"]
            self._set(**params)
        else:
            # the API cannot set an empty text, so park the region invisibly
            # (a single space at opacity 0); a divinus restart clears it fully
            self._set(text=" ", size=16, opal=0, color="#000", pos="0,0")
        self.dirty = False


# ---------------------------------------------------------------- measurements

def pct(arr, q) -> float:
    return float(np.percentile(np.asarray(arr, float), q))


class _Competitor:
    """A deliberately slow stream reader (background thread) that induces
    server-side backpressure while another stream's cadence is measured."""

    def __init__(self, ip: str, stream: str, recv_bytes: int = 2048, delay_s: float = 0.10):
        self.ip, self.stream, self.recv_bytes, self.delay_s = ip, stream, recv_bytes, delay_s
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)

    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                if self.stream == "mjpeg":
                    sock = socket.create_connection((self.ip, 80), timeout=10)
                    sock.sendall(f"GET /mjpeg HTTP/1.1\r\nHost: {self.ip}\r\n\r\n".encode())
                else:  # fmp4
                    sock = socket.create_connection((self.ip, 80), timeout=10)
                    sock.sendall(f"GET /video.mp4 HTTP/1.1\r\nHost: {self.ip}\r\n\r\n".encode())
                sock.settimeout(10)
                while not self._stop.is_set():
                    if not sock.recv(self.recv_bytes):
                        break
                    self._stop.wait(self.delay_s)
                sock.close()
            except OSError:
                self._stop.wait(0.5)

    def __enter__(self):
        self._thread.start()
        time.sleep(1.0)  # let the competitor attach and the buffer start filling
        return self

    def __exit__(self, *_exc):
        self._stop.set()
        self._thread.join(timeout=2)


def measure_cadence(stream: str, ip: str, seconds: float, warmup: int = 5,
                    read_timeout: float = 10.0) -> dict:
    src = make_source(stream, ip, max_seconds=seconds * 2 + 60, read_timeout=read_timeout)
    ts: list[float] = []
    t_end = None
    try:
        for t, _ in src:
            ts.append(t)
            if len(ts) == warmup + 1:
                t_end = t + seconds
            if t_end is not None and t >= t_end:
                break
    finally:
        src.close()
    if len(ts) < warmup + 5:
        raise MeasurementError(f"only {len(ts)} frames received")
    d = np.diff(ts[warmup:]) * 1000.0
    span = ts[-1] - ts[warmup]
    return {"frames": len(d) + 1, "span_s": round(span, 2),
            "fps": round(len(d) / span, 2),
            "mean_ms": round(float(d.mean()), 1),
            "stdev_ms": round(float(d.std()), 1),
            "p95_ms": round(pct(d, 95), 1),
            "max_ms": round(float(d.max()), 1),
            "samples_ms": [round(float(x), 2) for x in d]}


def measure_latency(stream: str, ip: str, osd: OsdController, n_samples: int) -> dict:
    it = gray_frames(stream, ip)
    try:
        for _ in range(3):  # warm up the connection / decoder
            next(it)

        def settle(seconds: float, keep: int = 3, hard: float = 12.0) -> list[float]:
            end = time.monotonic() + seconds
            stop = time.monotonic() + hard
            means: list[float] = []
            while True:
                _, g = next(it)
                means.append(window_mean(g))
                now = time.monotonic()
                if (now >= end and len(means) >= keep) or now >= stop:
                    return means[-keep:]

        # calibrate the two levels (black text vs white text, same glyphs)
        osd.apply(on=False)
        off_level = float(np.mean(settle(2.5)))
        osd.apply(on=True)
        on_level = float(np.mean(settle(2.5)))
        contrast = abs(on_level - off_level)
        if contrast < MIN_CONTRAST:
            raise MeasurementError(
                f"OSD toggle not detectable in {stream} (contrast {contrast:.1f} "
                f"gray levels, need >= {MIN_CONTRAST:.0f}); is the OSD visible?")
        mid = (on_level + off_level) / 2
        high_is_on = on_level > off_level

        samples: list[float] = []
        failures = 0
        state = True
        for _ in range(n_samples):
            target = not state
            t_toggle = osd.apply(on=target)
            deadline = t_toggle + SAMPLE_TIMEOUT_S
            latency_ms = None
            while time.monotonic() < deadline:
                t, g = next(it)
                on_side = (window_mean(g) > mid) == high_is_on
                if on_side == target:
                    latency_ms = (t - t_toggle) * 1000.0
                    break
            if latency_ms is None:
                failures += 1
                settle(1.5)  # let the state converge before the next toggle
            else:
                samples.append(latency_ms)
            state = target
        if not samples:
            raise MeasurementError(f"all {n_samples} toggles timed out on {stream}")
        return {"n": len(samples), "failures": failures,
                "contrast_gray": round(contrast, 1),
                "samples_ms": [round(s, 1) for s in samples],
                "min_ms": round(min(samples), 1),
                "p50_ms": round(pct(samples, 50), 1),
                "p95_ms": round(pct(samples, 95), 1),
                "max_ms": round(max(samples), 1)}
    finally:
        it.close()


# ---------------------------------------------------------------- reporting

def fmt_table(rows: list[list[str]], indent: str = "  ") -> str:
    widths = [max(len(r[i]) for r in rows) for i in range(len(rows[0]))]
    return "\n".join(indent + "  ".join(c.ljust(w) for c, w in zip(r, widths)).rstrip()
                     for r in rows)


def print_report(res: dict, out) -> None:
    p = lambda *a: print(*a, file=out)
    p(f"=== divinus latency measurement -- {res['camera']} -- {res['time']} ===")
    st, mp4, mj = res.get("status") or {}, res.get("mp4_cfg") or {}, res.get("mjpeg_cfg") or {}
    if st:
        p(f"camera : {st.get('chip', '?')} / {st.get('sensor', '?')}, uptime {st.get('uptime', '?')}")
    if mp4:
        p(f"mp4    : {'H265' if mp4.get('h265') else 'H264'} {mp4.get('width')}x{mp4.get('height')}"
          f" @ {mp4.get('fps')} fps, gop {mp4.get('gop')}, {mp4.get('mode')}"
          f" {mp4.get('bitrate')} kbps   (rtsp + fmp4 source)")
    if mj:
        p(f"mjpeg  : {mj.get('width')}x{mj.get('height')} @ {mj.get('fps')} fps, {mj.get('mode')}")
    p("")

    p(f"[latency] OSD inject-to-receive, {res['args']['samples']} toggles per stream (ms)")
    if not res["osd_available"]:
        p(f"  SKIPPED: {res['osd_detail']}")
    else:
        rows = [["stream", "n", "fail", "min", "p50", "p95", "max", "contrast"]]
        for s, r in res["latency_ms"].items():
            if "error" in r:
                rows.append([s, "-", "-", "-", "-", "-", "-", r["error"][:60]])
            else:
                rows.append([s, str(r["n"]), str(r["failures"]),
                             f"{r['min_ms']:.0f}", f"{r['p50_ms']:.0f}",
                             f"{r['p95_ms']:.0f}", f"{r['max_ms']:.0f}",
                             f"{r['contrast_gray']:.1f}"])
        p(fmt_table(rows))
        p("  note: every sample includes 0-1000 ms quantization from divinus' 1 Hz OSD")
        p("        poll (src/region.c: sleep(1)); 'min' is the comparable pipeline number.")
        p("        Sensor exposure/readout + ISP time is NOT included (~bounded by 1/fps + 3DNR).")
    p("")

    p(f"[cadence] frame-arrival intervals (ms), ~{res['args']['cadence_seconds']:.0f} s per stream")
    rows = [["stream", "frames", "fps", "mean", "stdev", "p95", "max", "expected"]]
    for s, r in res["cadence"].items():
        exp = (mj if s == "mjpeg" else mp4).get("fps")
        exp_s = f"{1000.0 / exp:.1f}" if exp else "?"
        if "error" in r:
            rows.append([s, "-", "-", "-", "-", "-", "-", r["error"][:60]])
        else:
            rows.append([s, str(r["frames"]), f"{r['fps']:.2f}", f"{r['mean_ms']:.1f}",
                         f"{r['stdev_ms']:.1f}", f"{r['p95_ms']:.1f}",
                         f"{r['max_ms']:.1f}", exp_s])
    p(fmt_table(rows))


# ---------------------------------------------------------------- main

def main() -> int:
    ap = argparse.ArgumentParser(
        description="Measure divinus end-to-end latency (OSD inject) and frame cadence.")
    ap.add_argument("ip", nargs="?", default="192.168.1.94", help="camera IP")
    ap.add_argument("--stream", choices=[*STREAMS, "all"], default="all")
    ap.add_argument("--samples", type=int, default=10, help="OSD toggles per stream")
    ap.add_argument("--json", action="store_true", help="machine-readable output on stdout")
    ap.add_argument("--cadence-seconds", type=float, default=10.0)
    ap.add_argument("--osd-id", type=int, default=9,
                    help="OSD slot to toggle (default 9 = least likely in use)")
    ap.add_argument("--no-cadence", action="store_true")
    ap.add_argument("--no-latency", action="store_true")
    ap.add_argument("--stall", choices=STREAMS, default=None,
                    help="run a deliberately slow consumer of this stream while measuring the "
                         "others' cadence (induces server backpressure; exposes send-path stalls)")
    ap.add_argument("--json-out", metavar="PATH",
                    help="also write the full JSON result to this file (for A/B plotting)")
    args = ap.parse_args()

    streams = list(STREAMS) if args.stream == "all" else [args.stream]
    human_out = sys.stderr if args.json else sys.stdout

    status = probe_json(args.ip, "/api/status")
    if status is None:
        print(f"ERROR: camera {args.ip} not reachable (GET /api/status failed)",
              file=sys.stderr)
        return 1

    res: dict = {
        "camera": args.ip,
        "time": datetime.now().isoformat(timespec="seconds"),
        "args": {"stream": args.stream, "samples": args.samples,
                 "cadence_seconds": args.cadence_seconds, "osd_id": args.osd_id},
        "status": status,
        "mjpeg_cfg": probe_json(args.ip, "/api/mjpeg"),
        "mp4_cfg": probe_json(args.ip, "/api/mp4"),
        "osd_available": False,
        "osd_detail": "",
        "latency_ms": {},
        "cadence": {},
    }

    osd = OsdController(args.ip, args.osd_id)
    res["osd_available"], res["osd_detail"] = osd.probe()

    if not args.no_latency:
        if not res["osd_available"]:
            print(f"[latency] skipped: {res['osd_detail']}", file=human_out)
        else:
            try:
                for s in streams:
                    print(f"[latency] {s}: calibrating + {args.samples} toggles...",
                          file=human_out)
                    try:
                        res["latency_ms"][s] = measure_latency(s, args.ip, osd, args.samples)
                    except (MeasurementError, ConnectionError, TimeoutError,
                            StopIteration, OSError) as e:
                        res["latency_ms"][s] = {"error": str(e) or type(e).__name__}
            finally:
                try:
                    osd.restore()
                except Exception as e:  # noqa: BLE001 - report, don't mask results
                    print(f"WARNING: OSD restore failed ({e}); "
                          f"slot {args.osd_id} may still show test text "
                          "(cleared by a divinus restart)", file=sys.stderr)

    if not args.no_cadence:
        for s in streams:
            note = f" (+slow {args.stall} competitor)" if args.stall else ""
            print(f"[cadence] {s}: sampling ~{args.cadence_seconds:.0f} s{note}...", file=human_out)
            try:
                # tolerate multi-second gaps when a competitor is starving the stream,
                # so the stall shows up as a large finite sample rather than an error
                rt = 30.0 if args.stall else 10.0
                if args.stall and args.stall != s:
                    with _Competitor(args.ip, args.stall):
                        res["cadence"][s] = measure_cadence(s, args.ip, args.cadence_seconds, read_timeout=rt)
                else:
                    res["cadence"][s] = measure_cadence(s, args.ip, args.cadence_seconds, read_timeout=rt)
            except (MeasurementError, ConnectionError, TimeoutError, OSError) as e:
                res["cadence"][s] = {"error": str(e) or type(e).__name__}

    res["args"]["stall"] = args.stall
    print("", file=human_out)
    print_report(res, human_out)
    if args.json:
        print(json.dumps(res, indent=2))
    if args.json_out:
        with open(args.json_out, "w") as f:
            json.dump(res, f, indent=2)
        print(f"[json] wrote {args.json_out}", file=human_out)

    hard_errors = [s for s, r in {**res["cadence"], **res["latency_ms"]}.items()
                   if "error" in r]
    return 1 if hard_errors else 0


if __name__ == "__main__":
    sys.exit(main())
