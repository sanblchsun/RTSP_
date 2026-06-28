# Desktop Streamer

Трансляция рабочего стола Windows через FFmpeg (gdigrab + libx264).

## Сборка

```
g++ -std=c++17 -I"C:\ffmpeg8_1_1\ffmpeg8_1_1\include" -c desktop_streamer.cpp -o desktop_streamer.o
g++ -std=c++17 -I"C:\ffmpeg8_1_1\ffmpeg8_1_1\include" -c main.cpp -o main.o
g++ -std=c++17 desktop_streamer.o main.o -L"C:\ffmpeg8_1_1\ffmpeg8_1_1\lib" ^
    -lavcodec -lavformat -lavutil -lavdevice -lswscale -lswresample ^
    -lws2_32 -lsecur32 -lbcrypt -lmfplat -lmfuuid -lstrmiids -lole32 -loleaut32 -luuid -lpthread ^
    -o desktop_streamer.exe
```

## Использование

### 1. Локальная сеть — UDP (минимальная задержка)

Стример (Windows, IP 192.168.88.75):
```
desktop_streamer.exe udp://192.168.88.127:8554
```

Клиент (Ubuntu, IP 192.168.88.127):
```
ffplay -fflags nobuffer -analyzeduration 0 udp://:8554
```

### 2. Локальная сеть — TCP

Стример **подключается** к клиенту:

Стример:
```
desktop_streamer.exe tcp://192.168.88.127:8554
```

Клиент (Ubuntu):
```
ffplay -listen 1 -fflags nobuffer -analyzeduration 0 tcp://0.0.0.0:8554
```

### 3. Через интернет/NAT — TCP

Стример подключается к клиенту (клиент слушает). На роутере **клиента** проброс TCP 8554 → его IP.

Стример:
```
desktop_streamer.exe tcp://<внешний_IP_клиента>:8554
```

Клиент:
```
ffplay -listen 1 -fflags nobuffer -analyzeduration 0 tcp://0.0.0.0:8554
```

### 4. Просмотр в браузере через WebRTC (Linux VPS)

Стример подключается к VPS:

**Стример (Windows):**
```
desktop_streamer.exe tcp://<VPS_IP>:8554
```

**VPS (Linux) — установка зависимостей:**
```bash
sudo apt update
sudo apt install libavformat-dev libavcodec-dev libavdevice-dev libavutil-dev libswresample-dev libswscale-dev
pip install fastapi uvicorn[standard] aiortc av
```

**VPS — запуск:**
```bash
python webrtc_server.py
```

**Браузер:** открыть `http://<VPS_IP>:8000`

Сервер принимает TCP-соединение от стримера и ретранслирует видео через WebRTC любому количеству зрителей.

## Настройка параметров

По умолчанию: 1920x1080, 30 FPS, 2 Mbps, ultrafast.

Изменить битрейт и другие параметры — в `main.cpp`, секция `EncodingSettings`.

## Выход из программы

**Ctrl+C** в окне стримера.
