# Desktop Streamer

Трансляция рабочего стола Windows в браузер или плеер через H.264 + WebRTC / RTSP.

## Архитектура

Два независимых режима работы:

### Режим 1: Push — через интернет на VPS, в браузер через WebRTC

```
┌──────────────────────┐     TCP/UDP      ┌───────────────────────┐     WebRTC     ┌─────────┐
│  Windows (агент)     │ ──────────────>  │  VPS (Linux)          │ ────────────>  │ Браузер │
│                      │  H.264 annex-B   │                       │                │         │
│  WGC → x264 →        │                   │  PyAV decode →        │                │  <video>│
│  TCP/UDP push        │                   │  aiortc WebRTC        │                │         │
│                      │                   │  FastAPI signalling   │                │         │
└──────────────────────┘                   └───────────────────────┘                └─────────┘
```

Агент подключается к VPS (outgoing TCP/UDP), передаёт сжатые H.264 NAL-единицы.
Сервер декодирует их в кадры и ретранслирует через WebRTC в браузер.
Подходит для работы через NAT — агенту нужен только исходящий доступ к VPS.

### Режим 2: RTSP — локально в плеер

```
┌──────────────────────┐    RTSP/RTP      ┌───────────┐
│  Windows (агент)     │ <──────────────  │  ffplay / │
│                      │     TCP          │  VLC      │
│  WGC → x264 →        │  interleaved     │           │
│  RTSP-сервер         │                  │           │
└──────────────────────┘                  └───────────┘
```

Агент сам является RTSP-сервером. Плеер подключается к нему, проходит RTSP-рукопожатие
и получает RTP-пакеты поверх того же TCP-соединения (interleaved mode).
Без VPS, без браузера — только Windows + плеер в одной сети.

---

## Детально: как формируется и передаётся видео

### Захват (WGC)

Windows.Graphics.Capture API (WinRT/C++):
- Создаётся D3D11-устройство, `Direct3D11CaptureFramePool` с форматом B8G8R8A8
- Для каждого монитора создаётся `GraphicsCaptureItem` и `GraphicsCaptureSession`
- `CaptureFrame()`: `pool.TryGetNextFrame()` — неблокирующий, возвращает null если экран не менялся
- Из кадра извлекается ID3D11Texture2D, копируется в staging-текстуру, пиксели читаются в CPU

### Кодирование (x264)

X264Encoder обёртка над libx264:
- BGRA → I420 (ручная конверсия с BT.601)
- Параметры: preset=ultrafast, tune=zerolatency, profile=main, CRF=23
- 30 FPS, `b_annexb=1` (старт-коды 00 00 00 01), `b_repeat_headers=1` (SPS/PPS в каждом IDR)
- `i_bframe=0`, `i_frame_reference=1`, `i_keyint_max=fps*2`
- SPS/PPS извлекаются из `x264_encoder_headers()` при инициализации

### Транспорт

Выход x264 — аннекс-B поток: `[00 00 00 01][NAL-заголовок][данные]...`

**TCP (push):** каждый access unit отправляется как:
```
[4 байта — размер (big-endian)][NAL data]
```
TCP_NODELAY, reconnect при обрыве.

**UDP (push):** фрагментация по 1392 байта на датаграмму:
```
[4 байта — seq (BE)][2 байта — frag_index (BE)][2 байта — total_frags (BE)][payload]
```
Сервер собирает по seq. Потеря фрагмента = потеря кадра. seq инкрементится на кадр.

**RTSP:** RTP-пакеты (RFC 3984) передаются interleaved поверх TCP:
```
[0x24][1 байт — channel][2 байта — длина (BE)][RTP-заголовок 12B][H.264 NAL]
```
Пакетизация H.264: Single NAL (≤1400B) или FU-A (>1400B).

---

## Детально: что происходит на сервере (push-режим)

### Приём

`TcpRelayReader` / `UdpRelayReader` — отдельные потоки-демоны:
- TCP: `accept()` → `recv(65536)` → парсинг [4B size][NAL] → `feed_nal()`
- UDP: `recvfrom()` → сборка фрагментов по seq → `feed_nal()`

### Декодирование

`H264StreamTrack.feed_nal()`:
- `av.CodecContext('h264').parse(nal_data)` → список пакетов
- `codec.decode(packet)` → список кадров (VideoFrame)
- Установка PTS (frame_count, time_base=1/30)
- Помещение в `asyncio.Queue(maxsize=60)`
- При переполнении очереди кадры дропаются (put_nowait → QueueFull)

### WebRTC

`H264StreamTrack.recv()` — `await queue.get()`, отдаёт кадр aiortc.
`MediaRelay.subscribe()` — мультиплексирование на нескольких клиентов.
Браузер создаёт SDP offer через `POST /offer`, сервер отвечает answer с H.264-треком.

---

## Детально: RTSP-сервер (локальный режим)

### Рукопожатие

Клиент (ffplay/VLC) инициирует RTSP over TCP:

1. **OPTIONS** → сервер отвечает списком методов
2. **DESCRIBE** → сервер генерирует SDP:
   - `m=video 0 RTP/AVP 96`
   - `a=rtpmap:96 H264/90000`
   - `a=fmtp:96 packetization-mode=1;profile-level-id=42C01F;sprop-parameter-sets=<base64 SPS>,<base64 PPS>`
3. **SETUP** → сервер выбирает transport (TCP interleaved, channel 0-1)
4. **PLAY** → сервер начинает отправку RTP-пакетов
5. **TEARDOWN** → завершение

SPS/PPS для SDP берутся из `x264_encoder_headers()`.

### RTP-пакетизация (RFC 3984)

`H264RtpPacketizer`:
- Разбор аннекс-B потока на NAL-единицы
- **Single NAL** (≤max_payload): `[RTP-заголовок 12B][NAL]`
- **FU-A** (>max_payload): `[RTP-заголовок][FU indicator][FU header][фрагмент]`

RTP-заголовок: version=2, payload_type=96, SSRC=0xDEADBEEF,
sequence инкрементится на каждый пакет, timestamp = 90000/30 на кадр.

### Interleaved framing

RTP-пакеты передаются поверх того же TCP-соединения (не через UDP):
```
[0x24][channel=0][2B length(BE)][RTP packet]
```
Порт 8554, один клиент, auto-reconnect.

---

## Компоненты: C++ агент

### `main.cpp`

Точка входа. Разбор аргументов, инициализация, главный цикл.

**Аргументы:**

| Команда | Режим |
|---------|-------|
| `desktop_streamer.exe` | RTSP-сервер (порт 8554) |
| `desktop_streamer.exe push <host>` | TCP push (порт 8554) |
| `desktop_streamer.exe push <host> <port>` | TCP push (указанный порт) |
| `desktop_streamer.exe push <host> <port> udp` | UDP push |
| `desktop_streamer.exe push <host> udp` | UDP push (порт 8554) |

**Основной цикл (push):**
```cpp
while (running) {
    capture->CaptureFrame(0, bgra, w, h)     // WGC захват
    encoder->EncodeFrame(bgra, nals)          // x264 кодирование
    pusher->SendNal(nals.data(), nals.size()) // TCP/UDP отправка
}
```

**Основной цикл (RTSP):**
```cpp
while (running) {
    capture->CaptureFrame(0, bgra, w, h)
    encoder->EncodeFrame(bgra, nals)
    packetizer.Packetize(nals, rtp_ts, rtp_packets) // RTP-пакетизация
    for (auto &pkt : rtp_packets)
        rtsp.SendRtp(pkt.data(), pkt.size())  // interleaved отправка
    rtp_ts += 90000/30
}
```

### `WGCCapture` (`capture_wgc.h/cpp`)

Захват экрана через Windows.Graphics.Capture:
- `Initialize(fps)` — D3D11, перечисление мониторов, создание FramePool и CaptureSession
- `CaptureFrame(monitor_id, bgra, w, h)` — `TryGetNextFrame()` → CopyResource → Map → memcpy
- `Shutdown()` — закрытие session/pool, освобождение D3D

### `X264Encoder` (`encoder_x264.h/cpp`)

Обёртка libx264:
- `Initialize(w, h, fps, qp)` — x264_param_default_preset("ultrafast","zerolatency"), apply_profile("main")
- `EncodeFrame(bgra, out_nal)` — BGRA→I420, `x264_encoder_encode`, concat NAL в out_nal
- `GetSps()`/`GetPps()` — параметры из `x264_encoder_headers`
- `Shutdown()` — `x264_encoder_close`

### `IPushSender` / `TcpPushSender` / `UdpPushSender`

Интерфейс и две реализации транспорта:
- `TcpPushSender` — blocking `send()`, TCP_NODELAY, reconnect
- `UdpPushSender` — `sendto()` с фрагментацией, без подтверждений

### `H264RtpPacketizer` (`rtp/h264_rtp_packetizer.h/cpp`)

RFC 3984: Single NAL и FU-A, RTP-заголовок 12 байт.

### `RtspServer` (`rtsp/rtsp_server.h/cpp`)

RTSP/1.0 сервер:
- `Start(port)` — listen TCP, поток AcceptLoop
- `HandleOptions/Describe/Setup/Play/Teardown` — обработка запросов
- `GenerateSdp()` — SDP с H.264 fmtp (profile-level-id + sprop-parameter-sets)
- `SendRtp(data, size)` — interleaved framing: `$channel length data`
- Один клиент, auto-kick предыдущего при новом подключении

---

## Компоненты: Python VPS

### `webrtc_server.py`

FastAPI + aiortc + PyAV.

### `H264StreamTrack`

`VideoStreamTrack` из aiortc:
- `feed_nal(nal_data)` — `codec.parse()` + `codec.decode()` → asyncio.Queue
- `recv()` — `await queue.get()` для WebRTC sender

### `TcpRelayReader`

Поток-демон: TCP-сервер на порту 8554, парсинг `[4B size][NAL]`, вызов `feed_nal`.

### `UdpRelayReader`

Поток-демон: UDP-сокет на порту 8554, сборка фрагментов по seq, вызов `feed_nal`.
Очистка зависших фрагментов через 5 сек.

### `UdpRelayReader._cleanup_stale()`

Удаляет незавершённые фрагменты старше 5 секунд (потерянные кадры).

### WebRTC signalling

- `GET /` — HTML-страница с WebRTC-клиентом (STUN, ICE restart)
- `POST /offer` — приём SDP offer, создание `RTCPeerConnection`, `addTrack(relay.subscribe(track))`, ответ answer
- `GET /health` — healthcheck

---

## Сборка (Windows)

Требования:
- Visual Studio 202x (x64) с компонентами "Разработка классических приложений на C++" и "C++/WinRT"
- [x264 SDK](https://www.videolan.org/developers/x264.html) (статическая библиотека) в `C:\x264_sdk`
- Windows 10 1903+ (WGC API)

```bash
cmake -B build64 -A x64
cmake --build build64 --config Release
```

Бинарник: `build64/Release/desktop_streamer.exe`

---

## Запуск

### VPS (push-режим)

```bash
pip install -r requirements.txt
# опционально: RELAY_PORT=8554 HTTP_PORT=8000
python webrtc_server.py
```

Сервер слушает TCP и UDP на порту 8554 одновременно.

### Агент (push-режим)

```cmd
desktop_streamer.exe push <VPS_IP>
desktop_streamer.exe push <VPS_IP> 8554 udp
```

### Агент (RTSP-режим)

```cmd
desktop_streamer.exe
```

Плеер:
```bash
ffplay -fflags nobuffer -flags low_delay rtsp://<AGENT_IP>:8554/stream
```
или VLC: `rtsp://<AGENT_IP>:8554/stream`

### Браузер (push-режим)

Открыть `http://<VPS_IP>:8000`

---

## Переменные окружения (сервер)

| Переменная | По умолч. | Описание |
|-----------|-----------|----------|
| `RELAY_PORT` | 8554 | Порт для TCP/UDP приёма H.264 |
| `HTTP_PORT` | 8000 | Порт веб-интерфейса (FastAPI) |

---

## Зависимости

### Агент (C++)
- libx264 (статическая)
- Windows.Graphics.Capture (WinRT, входит в Windows 10 1903+)
- D3D11, DXGI
- Winsock2

### Сервер (Python)
- `fastapi` + `uvicorn[standard]` — HTTP/WebSocket
- `aiortc` — WebRTC
- `av` (PyAV) — H.264 декодинг
