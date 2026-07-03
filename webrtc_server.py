"""
VPS WebRTC Relay Server
Accepts agent as RTSP client → interleaved RTP → H.264 → WebRTC → browser.
"""
import asyncio
import base64
from collections import deque
import fractions
import logging
import os
import socket
import struct
import threading
import time
from contextlib import asynccontextmanager

from fastapi import FastAPI, Request
from fastapi.responses import HTMLResponse, JSONResponse
import uvicorn
import av

from aiortc import RTCPeerConnection, RTCSessionDescription, VideoStreamTrack
from aiortc.contrib.media import MediaRelay

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)


class Diagnostics:
    def __init__(self, label="stream"):
        self._label = label
        self._lock = threading.Lock()
        self._last_report = 0.0
        self._report_interval = 10.0

        self._rtp_count = 0
        self._rtp_arrival_deltas = []
        self._rtp_seq_gaps = 0
        self._rtp_total_gaps = 0
        self._last_arrival = None
        self._last_seq = -1

        self._deque_overflow = 0
        self._deque_hits = 0

        self._pacing_wait_30_100 = 0
        self._pacing_wait_over_100 = 0
        self._pacing_samples = 0

        self._decode_errors = 0

    def on_rtp_arrival(self, now, seq):
        with self._lock:
            self._rtp_count += 1
            if self._last_arrival is not None:
                delta = (now - self._last_arrival) * 1000
                self._rtp_arrival_deltas.append(delta)
            self._last_arrival = now
            if self._last_seq >= 0:
                gap = (seq - self._last_seq - 1) & 0xFFFF
                if gap > 0:
                    self._rtp_seq_gaps += gap
                    self._rtp_total_gaps += 1
            self._last_seq = seq

    def on_frame_put(self, deque_len, maxlen):
        with self._lock:
            self._deque_hits += 1
            if deque_len >= maxlen:
                self._deque_overflow += 1

    def on_pacing_wait(self, wait_ms):
        with self._lock:
            self._pacing_samples += 1
            if wait_ms > 100:
                self._pacing_wait_over_100 += 1
            elif wait_ms > 30:
                self._pacing_wait_30_100 += 1

    def on_decode_error(self):
        with self._lock:
            self._decode_errors += 1

    def maybe_report(self):
        now = time.monotonic()
        with self._lock:
            if now - self._last_report < self._report_interval:
                return
            self._last_report = now

            if self._rtp_count == 0:
                return

            all_deltas = self._rtp_arrival_deltas

            net_delays = sum(1 for d in all_deltas if d > 50)
            net_stalls = sum(1 for d in all_deltas if d > 200)

            avg_arrival = sum(all_deltas) / len(all_deltas) if all_deltas else 0.0
            max_arrival = max(all_deltas) if all_deltas else 0.0

            logger.info(
                "[DIAG %s] frames=%d rtp=%d | "
                "net: avg_arr=%.1fms max_arr=%.0fms delays=%d stalls=%d | "
                "seq: gaps=%d events=%d | "
                "dec: errors=%d | "
                "deq: overflow=%d hits=%d | "
                "pace: over100=%d over30=%d samples=%d",
                self._label,
                self._deque_hits, self._rtp_count,
                avg_arrival, max_arrival, net_delays, net_stalls,
                self._rtp_seq_gaps, self._rtp_total_gaps,
                self._decode_errors,
                self._deque_overflow, self._deque_hits,
                self._pacing_wait_over_100, self._pacing_wait_30_100, self._pacing_samples,
            )

            self._rtp_arrival_deltas = []
            self._rtp_seq_gaps = 0
            self._rtp_total_gaps = 0
            self._deque_overflow = 0
            self._deque_hits = 0
            self._pacing_wait_30_100 = 0
            self._pacing_wait_over_100 = 0
            self._pacing_samples = 0
            self._decode_errors = 0

RTSP_PORT = int(os.environ.get("RTSP_PORT", "8554"))
HTTP_PORT = int(os.environ.get("HTTP_PORT", "8001"))

relay = MediaRelay()
source_track = None
_diag = None

# SPS/PPS extracted from the agent's H.264 stream, used for WebRTC signalling
_sps_data = None
_pps_data = None
_sps_pps_lock = threading.Lock()

# Reference to the current active agent session (for keyframe forwarding)
_agent_session = None
_agent_lock = threading.Lock()

HTML_PAGE = """\
<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8">
    <title>Desktop Stream</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body { background: #000; display: flex; justify-content: center; align-items: center; height: 100vh; font-family: sans-serif; }
        video { width: 100%; max-width: 1280px; }
        #status { position: fixed; top: 20px; right: 20px; color: #fff; background: rgba(0,0,0,0.7); padding: 8px 16px; border-radius: 4px; font-size: 14px; }
    </style>
</head>
<body>
    <div id="status">Connecting...</div>
    <video id="video" autoplay muted playsinline></video>
    <script>
        var pc = null;
        var video = document.getElementById('video');
        var status = document.getElementById('status');

        async function start() {
            status.textContent = 'Creating offer...';
            pc = new RTCPeerConnection({
                iceServers: [{ urls: 'stun:stun.l.google.com:19302' }]
            });

            pc.ontrack = function(ev) {
                if (ev.track.kind === 'video') {
                    video.srcObject = ev.streams[0];
                    status.textContent = 'Connected';
                }
            };

            pc.oniceconnectionstatechange = function() {
                var s = pc.iceConnectionState;
                status.textContent = s;
                if (s === 'failed' || s === 'disconnected') {
                    setTimeout(start, 2000);
                }
            };

            pc.onconnectionstatechange = function() {
                if (pc.connectionState === 'closed') {
                    setTimeout(start, 2000);
                }
            };

            var offer = await pc.createOffer({ offerToReceiveVideo: true });
            await pc.setLocalDescription(offer);

            var resp = await fetch('/offer', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ sdp: pc.localDescription.sdp, type: pc.localDescription.type })
            });
            var answer = await resp.json();
            await pc.setRemoteDescription(new RTCSessionDescription(answer));
        }
        start();
    </script>
</body>
</html>
"""


class H264StreamTrack(VideoStreamTrack):
    kind = "video"

    def __init__(self, loop):
        super().__init__()
        self._loop = loop
        self._deque = deque(maxlen=20)
        self._has_frames = asyncio.Event()
        self._running = True
        self._decode_lock = threading.Lock()
        self._codec = av.CodecContext.create('h264', 'r')
        self._codec.thread_count = 1
        self._request_keyframe_cb = None
        self._last_pts = None
        self._last_time = 0.0
        self._maxlen = 20

    def on_request_keyframe(self, cb):
        self._request_keyframe_cb = cb

    def _request_keyframe(self):
        logger.info("PLI/FIR requested from browser")
        if self._request_keyframe_cb:
            self._loop.call_soon_threadsafe(self._request_keyframe_cb)

    def stop(self):
        self._running = False

    def feed_nal(self, nal_data: bytes, rtp_timestamp: int):
        if not self._running:
            return
        # Extract SPS/PPS for WebRTC signalling (sprop-parameter-sets)
        global _sps_data, _pps_data
        if len(nal_data) > 4 and nal_data[:4] == START_CODE:
            nal_unit = nal_data[4:]
            nal_type = nal_unit[0] & 0x1F
            with _sps_pps_lock:
                if nal_type == 7:
                    _sps_data = bytes(nal_unit)
                elif nal_type == 8:
                    _pps_data = bytes(nal_unit)
        with self._decode_lock:
            try:
                for packet in self._codec.parse(nal_data):
                    for frame in self._codec.decode(packet):
                        if frame.width is None or frame.height is None:
                            continue
                        frame.pts = rtp_timestamp
                        frame.time_base = fractions.Fraction(1, 90000)
                        self._loop.call_soon_threadsafe(self._put, frame)
            except Exception as e:
                logger.warning("Decode error: %s", e)
                diag = _diag
                if diag:
                    diag.on_decode_error()

    def _put(self, frame):
        diag = _diag
        if diag:
            diag.on_frame_put(len(self._deque), self._maxlen)
        self._deque.append(frame)
        self._has_frames.set()

    async def recv(self):
        if not self._deque:
            self._has_frames.clear()
            starved_since = time.monotonic()
            while not self._deque:
                try:
                    await asyncio.wait_for(self._has_frames.wait(), timeout=0.05)
                except asyncio.TimeoutError:
                    starved_ms = (time.monotonic() - starved_since) * 1000
                    if starved_ms > 200:
                        logger.warning("Queue starved for %.0fms", starved_ms)
                        starved_since = time.monotonic()
            logger.info("Queue recovered after starvation")
        frame = self._deque.popleft()

        if self._last_pts is not None:
            pts_delta = (frame.pts - self._last_pts) / 90000
            if pts_delta < 0:
                pts_delta += (1 << 32) / 90000
            elapsed = time.monotonic() - self._last_time
            wait = pts_delta - elapsed
            if wait > 0.002:
                diag = _diag
                if diag:
                    diag.on_pacing_wait(wait * 1000)
                await asyncio.sleep(wait)

        self._last_pts = frame.pts
        self._last_time = time.monotonic()
        return frame


RTP_HEADER_SIZE = 12
START_CODE = b'\x00\x00\x00\x01'


class RtpParser:
    """Parses interleaved RTP → H.264 NALs → feed_nal."""

    def __init__(self):
        self._fua_buf = None
        self._fua_ts = 0
        self._last_arrival = None
        self._last_seq = -1

    def feed_rtp(self, data: bytes):
        global source_track, _diag
        if source_track is None:
            return
        if len(data) < RTP_HEADER_SIZE:
            return
        if (data[0] >> 6) != 2:
            return
        if (data[1] & 0x7F) != 96:
            return

        rtp_timestamp = struct.unpack('>I', data[4:8])[0]
        rtp_seq = struct.unpack('>H', data[2:4])[0]
        payload = data[RTP_HEADER_SIZE:]
        now = time.monotonic()

        if _diag:
            _diag.on_rtp_arrival(now, rtp_seq)
        if not payload:
            return

        nal_type = payload[0] & 0x1F

        try:
            if nal_type == 28:
                self._handle_fua(payload, rtp_timestamp)
            elif nal_type == 24:
                self._handle_stapa(payload, rtp_timestamp)
            elif nal_type <= 23:
                source_track.feed_nal(START_CODE + payload, rtp_timestamp)
        except Exception as e:
            logger.warning("RTP parse error: %s", e)

    def _handle_fua(self, payload: bytes, rtp_timestamp: int):
        header = payload[1]
        start = (header >> 7) & 1
        end = (header >> 6) & 1
        orig_type = header & 0x1F
        fragment = payload[2:]

        if start:
            if self._fua_buf is not None and self._fua_ts != rtp_timestamp:
                logger.warning("FU-A drop: buf=%d prev_ts=%d cur_ts=%d",
                               len(self._fua_buf), self._fua_ts, rtp_timestamp)
                self._fua_buf = None
            if self._fua_buf is None:
                self._fua_buf = bytearray()
                self._fua_ts = rtp_timestamp
                nal_header = bytes([(payload[0] & 0xE0) | orig_type])
                self._fua_buf.extend(START_CODE)
                self._fua_buf.extend(nal_header)
                self._fua_buf.extend(fragment)
                if end:
                    if source_track is not None:
                        source_track.feed_nal(bytes(self._fua_buf), self._fua_ts)
                    self._fua_buf = None
        elif self._fua_buf is not None:
            self._fua_buf.extend(fragment)
            if end:
                try:
                    if source_track is not None:
                        source_track.feed_nal(bytes(self._fua_buf), self._fua_ts)
                finally:
                    self._fua_buf = None
        elif end:
            self._fua_buf = None

    def _handle_stapa(self, payload: bytes, rtp_timestamp: int):
        offset = 1
        while offset + 2 <= len(payload):
            nalu_size = struct.unpack('>H', payload[offset:offset+2])[0]
            offset += 2
            if offset + nalu_size > len(payload):
                break
            if source_track is not None:
                source_track.feed_nal(START_CODE + payload[offset:offset+nalu_size], rtp_timestamp)
            offset += nalu_size


# === RTSP server (agent connection) ===

class AgentSession:
    """Handles one agent via RTSP + interleaved RTP."""

    def __init__(self, conn, addr, keyframe_cb=None):
        self._conn = conn
        self._addr = addr
        self._buf = b""
        self._running = True
        self._parser = RtpParser()
        self._play_sent = False
        self._keyframe_cb = keyframe_cb

    def request_keyframe(self):
        """Called when browser requests PLI/FIR — forward to agent."""
        try:
            self._conn.sendall(b"!K\r\n")
        except Exception:
            pass

    def close(self):
        self._running = False
        try:
            self._conn.close()
        except Exception:
            pass

    def handle(self):
        logger.info("Agent connected: %s", self._addr[0])
        session_id = "12345678"
        diag_report_counter = 0

        while self._running:
            diag_report_counter += 1
            if diag_report_counter % 300 == 0:
                diag = _diag
                if diag:
                    diag.maybe_report()

            try:
                data = self._conn.recv(65536)
            except Exception:
                break
            if not data:
                break

            self._buf += data

            while self._buf:
                # Try interleaved RTP ($ + channel + 2B length)
                if self._buf[0] == 0x24 and len(self._buf) >= 4:
                    pkt_len = struct.unpack('>H', self._buf[2:4])[0]
                    if len(self._buf) < 4 + pkt_len:
                        break
                    rtp = self._buf[4:4 + pkt_len]
                    self._buf = self._buf[4 + pkt_len:]
                    if self._play_sent:
                        self._parser.feed_rtp(rtp)
                    continue

                # Try RTSP request (terminated by \r\n\r\n)
                if b'\r\n\r\n' in self._buf:
                    raw, self._buf = self._buf.split(b'\r\n\r\n', 1)
                    request = raw.decode('utf-8', errors='replace')
                    lines = request.split('\r\n')
                    if not lines:
                        continue
                    parts = lines[0].split(' ')
                    if len(parts) < 2:
                        continue
                    method = parts[0]

                    cseq = 1
                    for line in lines[1:]:
                        if line.lower().startswith('cseq:'):
                            cseq = int(line.split(':')[1].strip())
                            break

                    if method == 'OPTIONS':
                        self._send(
                            f"RTSP/1.0 200 OK\r\n"
                            f"CSeq: {cseq}\r\n"
                            f"Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n\r\n"
                        )
                    elif method == 'DESCRIBE':
                        sdp = (
                            "v=0\r\no=- 0 1 IN IP4 0.0.0.0\r\ns=Desktop Stream\r\n"
                            "c=IN IP4 0.0.0.0\r\nt=0 0\r\n"
                            "m=video 0 RTP/AVP 96\r\n"
                            "a=rtpmap:96 H264/90000\r\n"
                            "a=fmtp:96 packetization-mode=1;profile-level-id=42C01F\r\n"
                            "a=control:trackID=0\r\n"
                        )
                        self._send(
                            f"RTSP/1.0 200 OK\r\n"
                            f"CSeq: {cseq}\r\n"
                            f"Content-Type: application/sdp\r\n"
                            f"Content-Length: {len(sdp)}\r\n\r\n{sdp}"
                        )
                    elif method == 'SETUP':
                        self._send(
                            f"RTSP/1.0 200 OK\r\n"
                            f"CSeq: {cseq}\r\n"
                            f"Transport: RTP/AVP/TCP;interleaved=0-1\r\n"
                            f"Session: {session_id}\r\n\r\n"
                        )
                    elif method == 'PLAY':
                        self._play_sent = True
                        self._send(
                            f"RTSP/1.0 200 OK\r\n"
                            f"CSeq: {cseq}\r\n"
                            f"Session: {session_id}\r\n"
                            f"RTP-Info: url=trackID=0\r\n\r\n"
                        )
                        logger.info("RTSP handshake complete, receiving RTP stream")
                    elif method == 'TEARDOWN':
                        self._send(f"RTSP/1.0 200 OK\r\nCSeq: {cseq}\r\n\r\n")
                        return
                    else:
                        self._send(f"RTSP/1.0 200 OK\r\nCSeq: {cseq}\r\n\r\n")
                else:
                    break

        logger.info("Agent disconnected: %s", self._addr[0])

    def _send(self, response: str):
        try:
            self._conn.sendall(response.encode("utf-8"))
        except Exception:
            self._running = False


class RtspServer:
    """Accepts agent TCP connection, runs RTSP handshake + RTP relay."""

    def __init__(self):
        self._running = False

    def stop(self):
        self._running = False

    def run(self):
        self._running = True
        server = None
        while self._running:
            try:
                server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                server.bind(('0.0.0.0', RTSP_PORT))
                server.listen(1)
                server.settimeout(1.0)
                logger.info("RTSP server on port %d (waiting for agent)", RTSP_PORT)

                while self._running:
                    try:
                        conn, addr = server.accept()
                        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                    except socket.timeout:
                        continue
                    global _agent_session
                    session = AgentSession(conn, addr)
                    with _agent_lock:
                        _agent_session = session
                    try:
                        session.handle()
                    finally:
                        with _agent_lock:
                            if _agent_session is session:
                                _agent_session = None

            except Exception as e:
                if self._running:
                    logger.error("RTSP error: %s", e)
                    time.sleep(1)
            finally:
                if server:
                    try:
                        server.close()
                    except Exception:
                        pass


# === FastAPI app ===

@asynccontextmanager
async def lifespan(app):
    global source_track, _diag
    loop = asyncio.get_running_loop()
    source_track = H264StreamTrack(loop)
    _diag = Diagnostics("stream")

    def on_keyframe_request():
        global _agent_session
        with _agent_lock:
            if _agent_session:
                _agent_session.request_keyframe()

    source_track.on_request_keyframe(on_keyframe_request)

    rtsp = RtspServer()
    threading.Thread(target=rtsp.run, daemon=True).start()
    logger.info("VPS ready — RTSP on %d, WebRTC on %d", RTSP_PORT, HTTP_PORT)
    yield
    source_track.stop()
    rtsp.stop()


app = FastAPI(title="VPS Desktop Stream — WebRTC", lifespan=lifespan)


@app.get("/", response_class=HTMLResponse)
async def index():
    return HTML_PAGE


@app.post("/offer")
async def offer(request: Request):
    data = await request.json()
    offer = RTCSessionDescription(sdp=data["sdp"], type=data["type"])

    pc = RTCPeerConnection()

    if source_track:
        pc.addTrack(relay.subscribe(source_track))
    else:
        return JSONResponse(status_code=503, content={"error": "No source track available"})

    @pc.on("connectionstatechange")
    async def on_connection_state():
        if pc.connectionState in ("failed", "closed"):
            await pc.close()

    async def log_webrtc_stats():
        while True:
            await asyncio.sleep(5)
            try:
                stats = await pc.getStats()
                for s in stats.values():
                    if s.type == 'inbound-rtp' and s.kind == 'video':
                        lost = getattr(s, 'packetsLost', 0)
                        jitter_ms = getattr(s, 'jitter', 0) * 1000
                        rtt = getattr(s, 'roundTripTime', None)
                        rtt_ms = rtt * 1000 if rtt else 0
                        logger.info(
                            "[WEBRTC] packetsLost=%d jitter=%.1fms rtt=%.1fms",
                            lost, jitter_ms, rtt_ms,
                        )
            except Exception:
                break

    asyncio.create_task(log_webrtc_stats())

    await pc.setRemoteDescription(offer)
    answer = await pc.createAnswer()
    await pc.setLocalDescription(answer)

    sdp = pc.localDescription.sdp

    # Inject sprop-parameter-sets into answer SDP so the browser can decode
    # without waiting for an in-band IDR (critical for b_intra_refresh mode)
    with _sps_pps_lock:
        sps = _sps_data
        pps = _pps_data
    if sps and pps:
        sprop = "{},{}".format(base64.b64encode(sps).decode(), base64.b64encode(pps).decode())
        lines = sdp.split("\r\n")
        for i, line in enumerate(lines):
            if line.startswith("a=fmtp:96") and "sprop-parameter-sets" not in line:
                lines[i] = line + ";sprop-parameter-sets=" + sprop
                break
        sdp = "\r\n".join(lines)

    return {"sdp": sdp, "type": pc.localDescription.type}


@app.get("/health")
async def health():
    return {"status": "online"}


if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=HTTP_PORT)
