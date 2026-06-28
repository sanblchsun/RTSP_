import asyncio
import logging
import os
import threading
import time
from contextlib import asynccontextmanager

from fastapi import FastAPI, Request
from fastapi.responses import HTMLResponse
import uvicorn

from aiortc import RTCPeerConnection, RTCSessionDescription, VideoStreamTrack
from aiortc.contrib.media import MediaRelay
import av

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

STREAM_URL = os.environ.get(
    "STREAM_URL",
    "tcp://0.0.0.0:8554?listen=1&reuse=1",
)
HTTP_PORT = int(os.environ.get("HTTP_PORT", "8000"))

relay = MediaRelay()
source_track = None

HTML_PAGE = """\
<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8">
    <title>Desktop Streamer</title>
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


class StreamTrack(VideoStreamTrack):
    kind = "video"

    def __init__(self, loop):
        super().__init__()
        self._loop = loop
        self._queue = asyncio.Queue(maxsize=60)
        self._running = True
        self._thread = threading.Thread(target=self._reader, daemon=True)

    def start(self):
        self._thread.start()

    def stop(self):
        self._running = False

    def _reader(self):
        url = STREAM_URL
        while self._running:
            container = None
            try:
                logger.info("Opening stream: %s", url)
                container = av.open(url)
                logger.info("Stream opened")

                for packet in container.demux(video=0):
                    for frame in packet.decode():
                        if not self._running:
                            return
                        if frame.pts is None:
                            continue

                        out = frame.reformat()
                        out.pts = frame.pts
                        out.time_base = frame.time_base

                        self._loop.call_soon_threadsafe(self._put, out)

                logger.info("Stream ended, reconnecting...")
            except Exception as e:
                if self._running:
                    logger.error("Stream error: %s", e)
                    time.sleep(1)
            finally:
                if container:
                    try:
                        container.close()
                    except:
                        pass

    def _put(self, frame):
        try:
            self._queue.put_nowait(frame)
        except asyncio.QueueFull:
            pass

    async def recv(self):
        return await self._queue.get()


@asynccontextmanager
async def lifespan(app):
    global source_track
    loop = asyncio.get_running_loop()
    source_track = StreamTrack(loop)
    source_track.start()
    logger.info("Server ready, stream URL: %s", STREAM_URL)
    yield
    if source_track:
        source_track.stop()


app = FastAPI(title="Desktop Streamer — WebRTC", lifespan=lifespan)


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
    if source_track and not source_track._queue.empty():
        return {"status": "streaming"}
    return {"status": "waiting"}


if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=HTTP_PORT)
