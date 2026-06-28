# Desktop Streamer

Трансляция рабочего стола Windows в браузер через WebRTC.

`desktop_streamer.exe` — захват экрана (gdigrab) → H.264 → MPEG-TS → TCP/UDP.
`webrtc_server.py` — FastAPI + aiortc, принимает поток и ретранслирует в браузер.

## Быстрый старт

**Windows (стример):**
```
desktop_streamer.exe tcp://<VPS_IP>:8554
```

**Linux VPS (сервер):**
```bash
sudo apt install libavformat-dev libavcodec-dev libavdevice-dev libavutil-dev libswresample-dev libswscale-dev
pip install fastapi uvicorn[standard] aiortc av

STREAM_URL="tcp://0.0.0.0:8554?listen=1&reuse=1" python webrtc_server.py
```

**Браузер:** `http://<VPS_IP>:8000`

## Режимы транспорта

Управляются переменной `STREAM_URL` на сервере и соответствующим URL стримера.

| Транспорт | Стример (Windows) | Сервер (VPS) |
|-----------|------------------|--------------|
| TCP | `tcp://<VPS>:8554` | `STREAM_URL="tcp://0.0.0.0:8554?listen=1&reuse=1"` |
| UDP | `udp://<VPS>:8554` | `STREAM_URL="udp://:8554"` |

TCP — гарантированная доставка, автоматическое переподключение при разрыве (до 300 попыток).
UDP — меньше накладных расходов, без подтверждений.

**Переменные сервера:**
- `STREAM_URL` — URL входящего потока (по умолчанию `tcp://0.0.0.0:8554?listen=1&reuse=1`)
- `HTTP_PORT` — порт веб-интерфейса (по умолчанию `8000`)

## Локальный просмотр без VPS

Стример + ffplay напрямую (без браузера).

**UDP (минимальная задержка):**

Стример: `desktop_streamer.exe udp://192.168.88.127:8554`
Клиент: `ffplay -fflags nobuffer -analyzeduration 0 udp://:8554`

**TCP:**

Стример: `desktop_streamer.exe tcp://192.168.88.127:8554`
Клиент: `ffplay -listen 1 -fflags nobuffer -analyzeduration 0 tcp://0.0.0.0:8554`

**Через интернет (NAT):**

Проброс порта 8554 на роутере клиента. Клиент слушает:
```
ffplay -listen 1 -fflags nobuffer -analyzeduration 0 tcp://0.0.0.0:8554
```
Стример подключается:
```
desktop_streamer.exe tcp://<внешний_IP_клиента>:8554
```

## Сборка

Требуется FFmpeg 8.1.1 (shared) в `C:\ffmpeg8_1_1\ffmpeg8_1_1\`.

```
g++ -std=c++17 -I"C:\ffmpeg8_1_1\ffmpeg8_1_1\include" -c desktop_streamer.cpp -o desktop_streamer.o
g++ -std=c++17 -I"C:\ffmpeg8_1_1\ffmpeg8_1_1\include" -c main.cpp -o main.o
g++ -std=c++17 desktop_streamer.o main.o -L"C:\ffmpeg8_1_1\ffmpeg8_1_1\lib" ^
    -lavcodec -lavformat -lavutil -lavdevice -lswscale -lswresample ^
    -lws2_32 -lsecur32 -lbcrypt -lmfplat -lmfuuid -lstrmiids -lole32 -loleaut32 -luuid -lpthread ^
    -o desktop_streamer.exe
```

## Настройка параметров

По умолчанию: 1920×1080, 30 FPS, 2 Mbps, ultrafast, baseline.
Изменить — в `main.cpp`, секция `EncodingSettings`.

## Выход

**Ctrl+C** в окне стримера.
