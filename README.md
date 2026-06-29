# Desktop Streamer

Трансляция рабочего стола Windows в браузер через H.264 + RTSP/RTP + WebRTC.

Захват экрана (WGC) → кодирование (x264) → RTP-пакетизация (RFC 3984) → RTSP transport → WebRTC.

---

## Быстрый старт

**1. VPS (Linux):**
```bash
pip install fastapi uvicorn[standard] aiortc av
python webrtc_server.py
```
Сервер слушает порт 8554 (RTSP) и 8000 (WebRTC + веб-страница).

**2. Windows (агент):**
```cmd
desktop_streamer.exe push <VPS_IP>
```
Подключается к VPS, проходит RTSP-рукопожатие, начинает стриминг.

**3. Браузер:** открыть `http://<VPS_IP>:8000`

---

## Архитектура

```
┌─────────────────────────┐     RTSP + interleaved RTP     ┌───────────────────────────┐     WebRTC     ┌─────────┐
│  Windows (агент)        │ ──────────────────────────────> │  VPS (Linux, Python)      │ ────────────>  │ Браузер │
│                         │   TCP :8554                    │                           │                │         │
│  WGC → x264 →           │   OPTIONS/DESCRIBE/SETUP/PLAY │  RTSP-сервер →            │                │  <video>│
│  RTP-пакетизатор →      │   $channel length RTP         │  RTP-парсер → PyAV →      │                │         │
│  RTSP-клиент            │                                │  aiortc WebRTC             │                │         │
└─────────────────────────┘                                └───────────────────────────┘                └─────────┘
```

Агент = RTSP-клиент. VPS = RTSP-сервер. После RTSP-рукопожатия видео передаётся как interleaved RTP (RFC 3984) по тому же TCP-соединению. Никаких UDP, никаких сырых NAL.

---

## Режимы работы

### 1. Push на VPS (основной)

Агент подключается к VPS (outgoing TCP), проходит RTSP-рукопожатие и передаёт поток. VPS отдаёт видео через WebRTC в браузер. Единственный порт для входящих подключений — 8000 (HTTP). Агент сам инициирует соединение, поэтому NAT/фаервол не мешают.

### 2. RTSP-сервер (локальный)

```cmd
desktop_streamer.exe
```

Агент сам становится RTSP-сервером. Любой RTSP-клиент (ffplay, VLC) подключается и получает поток.
```
ffplay -fflags nobuffer rtsp://<IP_АГЕНТА>:8554/stream
```

---

## Как устроено

### Захват (WGC)

Windows.Graphics.Capture API (WinRT/C++):
- D3D11-устройство, `Direct3D11CaptureFramePool` с форматом B8G8R8A8
- `TryGetNextFrame()` — неблокирующий захват, возвращает null если кадр не изменился
- Копирование текстуры в CPU через staging-ресурс и Map/Unmap

### Кодирование (x264)

Статически слинкованный libx264 (без DLL):
- BGRA → I420 (BT.601)
- `preset=ultrafast`, `tune=zerolatency`, `profile=main`, CRF=23
- 30 FPS, `b_annexb=1`, `b_repeat_headers=1` (SPS/PPS в каждом IDR)
- Без B-фреймов, один ref-frame, keyint = 2 секунды

### RTP-пакетизация (RFC 3984)

`H264RtpPacketizer`:
- Разбор annex-B потока на NAL-единицы (поиск старт-кодов `00 00 00 01`)
- **Single NAL** (≤1400 байт): `[RTP-заголовок 12 байт][NAL-единица]`
- **FU-A** (>1400 байт): фрагментация крупных NAL на части, каждая со своим FU indicator + FU header
- RTP-заголовок: version=2, payload_type=96, SSRC=0xDEADBEEF, sequence инкрементится на каждый пакет, timestamp = 90000/30 на кадр

### RTSP-рукопожатие (агент → VPS)

Агент отправляет последовательно:
1. `OPTIONS * RTSP/1.0` → сервер отвечает списком методов
2. `DESCRIBE rtsp://relay/stream` → сервер отвечает SDP с H.264
3. `SETUP rtsp://relay/stream/trackID=0` → сервер выбирает transport (`RTP/AVP/TCP;interleaved=0-1`)
4. `PLAY rtsp://relay/stream` → сервер подтверждает готовность

После PLAY агент начинает отправлять interleaved RTP:
```
[0x24][channel=0][2 байта длина (big-endian)][RTP-пакет]
```

### Декодирование на сервере (Python)

`RtpParser`: разбор RTP-пакетов (Single NAL, FU-A, STAP-A), извлечение H.264 NAL с восстановлением старт-кодов.
`H264StreamTrack.feed_nal()`: PyAV `codec.parse()` + `codec.decode()` → asyncio.Queue.
`H264StreamTrack.recv()`: отдаёт кадры aiortc для WebRTC.

### WebRTC

- `GET /` — одностраничный HTML с WebRTC-клиентом (STUN, ICE restart при обрыве)
- `POST /offer` — SDP offer от браузера → сервер создаёт RTCPeerConnection, добавляет H.264-трек, отвечает answer
- `MediaRelay` — мультиплексирование на нескольких клиентов
- ICE: STUN-сервер `stun:stun.l.google.com:19302`

---

## Детали реализации

### C++ агент (Windows)

| Компонент | Файл | Назначение |
|-----------|------|------------|
| `main.cpp` | `main.cpp` | Аргументы, инициализация, главный цикл, `RtspClient` |
| `WGCCapture` | `WinRT-API/capture_wgc.h/cpp` | Захват экрана через WinRT |
| `X264Encoder` | `WinRT-API/encoder_x264.h/cpp` | H.264-кодирование, статический x264 |
| `H264RtpPacketizer` | `rtp/h264_rtp_packetizer.h/cpp` | RFC 3984: Single NAL, FU-A |
| `RtspServer` | `rtsp/rtsp_server.h/cpp` | RTSP-сервер (локальный режим) |
| `RtspClient` | `main.cpp` | RTSP-клиент (push-режим) |

`RtspClient`: подключается по TCP, проходит OPTIONS/DESCRIBE/SETUP/PLAY, шлёт RTP в формате `$channel length data`.

### Python сервер (VPS)

`webrtc_server.py` — всё в одном файле:

| Класс | Назначение |
|-------|------------|
| `H264StreamTrack` | VideoStreamTrack aiortc, приём NAL → PyAV → asyncio.Queue |
| `RtpParser` | Разбор RTP-пакетов, извлечение H.264 (Single NAL, FU-A, STAP-A) |
| `AgentSession` | Обработка одного агента: RTSP-рукопожатие, приём interleaved RTP |
| `RtspServer` | TCP-сервер (поток-демон), принимает агента, запускает AgentSession |

### Формат на проводе (push + WebRTC)

```
[TCP соединение]
├── RTSP: OPTIONS / DESCRIBE / SETUP / PLAY (текстовые запросы-ответы)
└── после PLAY:
    └── [0x24][0x00][длина BE][RTP-пакет 12 байт + H.264 payload] × N
```

RTP payload — H.264 в соответствии с RFC 3984:
- PT=96, SSRC=0xDEADBEEF
- Single NAL для мелких NAL (≤1400 байт)
- FU-A для крупных NAL (фрагментация)

---

## Сборка (Windows)

Требования:
- Visual Studio 202x (x64) с компонентами «Разработка классических приложений на C++» и «C++/WinRT»
- Статическая libx264 в `C:\x264_sdk`
- Windows 10 1903+ (WGC API)

```bash
cmake -B build64 -A x64
cmake --build build64 --config Release
```

Бинарник: `build64/Release/desktop_streamer.exe`

### Структура `C:\x264_sdk`
```
C:\x264_sdk\
├── include\
│   └── x264.h
├── lib\
│   └── libx264.lib
```

---

## Запуск

### VPS
```bash
python webrtc_server.py
```
Порты: 8554 (RTSP), 8000 (HTTP + WebRTC).

Переменные окружения:
- `RTSP_PORT` — порт для RTSP (по умолч. 8554)
- `HTTP_PORT` — порт для HTTP/WebRTC (по умолч. 8000)

### Агент (push на VPS)
```cmd
desktop_streamer.exe push <VPS_IP>
desktop_streamer.exe push <VPS_IP> 8554
```

### Агент (локальный RTSP-сервер)
```cmd
desktop_streamer.exe
```
Плеер: `ffplay -fflags nobuffer rtsp://<IP>:8554/stream`

### Браузер
Открыть `http://<VPS_IP>:8000`

---

## Зависимости

### Агент (C++)
- libx264 (статическая)
- Windows.Graphics.Capture (WinRT, входит в Windows 10 1903+)
- D3D11, DXGI
- Winsock2

### Сервер (Python)
- `fastapi` + `uvicorn[standard]` — HTTP
- `aiortc` — WebRTC
- `av` (PyAV) — H.264 декодинг
