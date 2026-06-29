"""
VPS WebRTC Relay Server
Receives H.264 NALs over TCP and relays via WebRTC to browsers.

Agent network protocol:
  [4-byte NAL size (big-endian)][NAL data (annex-B)]...
"""
import asyncio
import fractions
import logging
import os
import socket
import struct
import threading
import time
from contextlib import asynccontextmanager

from fastapi import FastAPI, Request
from fastapi.responses import HTMLResponse
import uvicorn
import av

from aiortc import RTCPeerConnection, RTCSessionDescription, VideoStreamTrack
from aiortc.contrib.media import MediaRelay

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

RELAY_PORT = int(os.environ.get("RELAY_PORT", "8554"))
HTTP_PORT = int(os.environ.get("HTTP_PORT", "8000"))

relay = MediaRelay()
source_track = None

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
        self._queue = asyncio.Queue(maxsize=60)
        self._running = True
        self._reader_thread = None
        self._frame_count = 0
        self._last_log = 0
        # H.264 decoder
        self._codec = av.CodecContext.create('h264', 'r')
        self._codec.extradata = None
        self._codec.thread_count = 0

    def start(self, reader):
        self._reader_thread = threading.Thread(target=reader.run, daemon=True)
        self._reader_thread.start()

    def stop(self):
        self._running = False

    def feed_nal(self, nal_data: bytes):
        if not self._running:
            return
        t0 = time.monotonic()
        n_frames = 0
        try:
            for packet in self._codec.parse(nal_data):
                for frame in self._codec.decode(packet):
                    if frame.width is None or frame.height is None:
                        continue
                    frame.pts = self._frame_count
                    frame.time_base = fractions.Fraction(1, 30)
                    self._frame_count += 1
                    n_frames += 1
                    self._loop.call_soon_threadsafe(self._put, frame)
        except Exception as e:
            logger.warning("Decode error: %s", e)
        elapsed_ms = (time.monotonic() - t0) * 1000
        if n_frames > 0 and elapsed_ms > 10:
            logger.info("feed_nal: %.1fms, frames=%d, dec=%d, q=%d",
                        elapsed_ms, self._frame_count, n_frames, self._queue.qsize())


    def _put(self, frame):
        try:
            self._queue.put_nowait(frame)
        except asyncio.QueueFull:
            pass

    async def recv(self):
        return await self._queue.get()


class TcpRelayReader:
    """Reads H.264 NALs from TCP and feeds to track."""

    def __init__(self, track: H264StreamTrack):
        self._track = track
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
                server.bind(('0.0.0.0', RELAY_PORT))
                server.listen(1)
                server.settimeout(5.0)
                logger.info("Relay listening on port %d", RELAY_PORT)

                while self._running:
                    try:
                        conn, addr = server.accept()
                    except socket.timeout:
                        continue
                    logger.info("Agent connected: %s", addr[0])
                    conn.settimeout(None)

                    # Disable Nagle
                    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

                    buf = b""
                    while self._running:
                        try:
                            data = conn.recv(65536)
                        except Exception:
                            break
                        if not data:
                            break

                        buf += data
                        # Parse: [4-byte BE size][NAL data]
                        while len(buf) >= 4:
                            size = struct.unpack('>I', buf[:4])[0]
                            if size == 0:
                                buf = buf[4:]
                                continue
                            if len(buf) < 4 + size:
                                break
                            nal = buf[4:4 + size]
                            buf = buf[4 + size:]
                            self._track.feed_nal(nal)

                    logger.info("Agent disconnected")
                    conn.close()

            except Exception as e:
                if self._running:
                    logger.error("Relay error: %s", e)
                    time.sleep(1)
            finally:
                if server:
                    try:
                        server.close()
                    except Exception:
                        pass


@asynccontextmanager
async def lifespan(app):
    global source_track
    loop = asyncio.get_running_loop()
    source_track = H264StreamTrack(loop)
    reader = TcpRelayReader(source_track)
    source_track.start(reader)
    logger.info("VPS relay ready on port %d", RELAY_PORT)
    yield
    source_track.stop()
    reader.stop()


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
        return {"error": "No source track available"}, 503

    @pc.on("connectionstatechange")
    async def on_connection_state():
        if pc.connectionState in ("failed", "closed"):
            await pc.close()

    await pc.setRemoteDescription(offer)
    answer = await pc.createAnswer()
    await pc.setLocalDescription(answer)

    return {"sdp": pc.localDescription.sdp, "type": pc.localDescription.type}


@app.get("/health")
async def health():
    return {"status": "online"}


if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=HTTP_PORT)
