# План: замена worker в sysdm-build на push-модуль из RTSP_

## Цель
Заменить старый RDP worker (WebSocket + ingest) в `sysdm-build` на push-клиент из `RTSP_` (WGC → x264 → RTP/RTSP/TCP). Временная отправная точка для интеграции RTSP_ в sysdm/web-relay/builder_cpp.

## Схема после изменений

```
Windows Agent (exe --rdp-worker)
  WGC Capture → x264 Encode → RTP Packetize → RTSP/TCP
       │
       ▼ TCP:8555
VPS (webrtc_server.py)
  RtspServer → RtpParser → PyAV Decode → MediaRelay → WebRTC
       │
       ▼
Браузер https://dev.local:8002/
```

## Текущая архитектура (sysdm-build)

```
main.cpp (service)
  → spawnRDPWorker()
    → CreateProcessAsUserA(agent.exe --rdp-worker --server=X --port=Y --id=... --token=... --shm=...)
      → run_rdp_worker() [rdp_agent.cpp]
        → WebSocket relay → capture → encode → ingest
```

## Новая архитектура

```
main.cpp (service)
  → spawnRDPWorker()
    → CreateProcessAsUserA(agent.exe --rdp-worker --server=X --port=Y ...)
      → run_rtsp_push_worker() [rtsp_push_worker.cpp]
        → WGC capture → x264 → RTP → RTSP/TCP → VPS:8555
```

## Список изменений

### 1. Удалить (11 файлов из builder_cpp/agent/cmd/agent/)

| Файл | Причина |
|------|---------|
| `rdp_agent.cpp` | Старый worker (WebSocket + ingest) |
| `rdp_agent.h` | Использует capture_base.h, encoder.h |
| `capture_base.h` | Абстрактный базовый класс (не нужен) |
| `capture_dda.cpp`, `capture_dda.h` | DDA захват (не используется) |
| `encoder.h` | Абстрактный базовый класс (не нужен) |
| `encoder_amf.cpp`, `encoder_amf.h` | AMF (не используется) |
| `encoder_ave.cpp`, `encoder_ave.h` | AVE (не используется) |
| `encoder_qsv.cpp`, `encoder_qsv.h` | QSV (не используется) |

### 2. Добавить (3 RTP-файла из RTSP_)

| Файл (куда: sysdm-build) | Откуда: RTSP_ | Изменения |
|-------------------------|---------------|-----------|
| `cmd/agent/rtp/rtp_header.h` | `rtp/rtp_header.h` | Без изменений |
| `cmd/agent/rtp/h264_rtp_packetizer.h` | `rtp/h264_rtp_packetizer.h` | Без изменений |
| `cmd/agent/rtp/h264_rtp_packetizer.cpp` | `rtp/h264_rtp_packetizer.cpp` | Без изменений |

### 3. Заменить (4 файла — взять standalone-версии из RTSP_)

| Файл (sysdm → RTSP_) | Различия |
|----------------------|----------|
| `capture_wgc.h` | sysdm: `class WGCCapture : public CaptureBase` → RTSP_: standalone class |
| `capture_wgc.cpp` | Аналогично, убрать наследование от CaptureBase |
| `encoder_x264.h` | sysdm: `class X264Encoder : public IEncoder` → RTSP_: standalone + `RequestKeyframe()`, `GetSps()`, `GetPps()` |
| `encoder_x264.cpp` | Аналогично, standalone, + bgra_to_i420, keyframe, сохранение sps/pps |

### 4. Создать (1 новый парный файл)

**`rtsp_push_worker.h`**:
```cpp
#pragma once
#include <string>
int run_rtsp_push_worker(
    const std::string &server_host, int server_port,
    const std::string &agent_id, const std::string &agent_token,
    bool verify_cert, int timeout_min, const std::string &shm_name,
    const std::string &codec, const std::string &encoder,
    const std::string &quality, int fps);
```

**`rtsp_push_worker.cpp`** — копия `RTSP_/main.cpp` (строки 205–348) с адаптацией:
- Сигнатура функции: `run_rtsp_push_worker(...)` вместо `main()`
- Убрать обработчики сигналов (SIGINT/SIGTERM) — main убивает через TerminateProcess
- `kRtspPort = 8555` — жёсткая константа
- `--server=` → VPS-хост для RTSP/TCP пуша
- `logf()` — extern, определена в main.cpp
- `RtspClient` — inline-класс, остаётся внутри файла без изменений

### 5. Изменить main.cpp (sysdm-build)

- Убрать `#include "rdp_agent.h"`, `"encoder_amf.h"`, `"encoder_ave.h"`, `"encoder_qsv.h"`
- Добавить `#include "rtsp_push_worker.h"`
- Строка 3310: `run_rdp_worker(...)` → `run_rtsp_push_worker(...)`
- Убрать создание `ActivityShm` в `spawnRDPWorker()`
- Убрать `inactivity_monitor_thread()` и связанные переменные

### 6. Изменить build_agents.py

- Убрать: `CPP_RDP_AGENT`, `CPP_CAPTURE_DDA`, `CPP_ENCODER_AMF`, `CPP_ENCODER_AVE`, `CPP_ENCODER_QSV`
- Добавить:
  ```python
  CPP_RTSP_PUSH = CPP_AGENT_DIR / "cmd" / "agent" / "rtsp_push_worker.cpp"
  CPP_RTP_PACKETIZER = CPP_AGENT_DIR / "cmd" / "agent" / "rtp" / "h264_rtp_packetizer.cpp"
  ```

## Ожидаемый тест

1. Запустить `webrtc_server.py` на VPS (порты 8555/8002)
2. Собрать и запустить агента на Windows
3. Отправить `start-rdp-worker` → main процесс создаёт worker с `--server=<VPS_IP>`
4. Worker пушит RTSP видео на VPS:8555
5. Открыть `http://<VPS_IP>:8002/` в браузере → видео рабочего стола

## Критерий успеха

Видео рабочего стола в браузере по `http://<VPS_IP>:8002/` (мышка/клавиатура/команды не нужны — это будет следующий этап).
