# План интеграции: перенос функционала управления удалённым рабочим столом из sysdm в RTSP_

## Цель

Перенести из проекта `sysdm` в проект `RTSP_` всё, что касается удалённого управления
рабочим столом: Windows-сервис, два процесса (main + worker), мышь/клавиатура,
автообновление. После завершения `sysdm` остаётся только проектом управления
и обеспечения безопасности (dashboard не переносится).

**Приоритет**: код `RTSP_` (кодирование, транспорт, декодирование, доставка видео)
является эталоном. При конфликте берётся версия из `RTSP_`.

---

## 1. Общая архитектура

```
                        VPS (Debian, systemd)
┌─────────────────────────────────────────────────────────────────────┐
│  webrtc_server.py                                                    │
│  ┌─────────────┐  ┌──────────────────┐  ┌────────────────────────┐ │
│  │ RTSP Server  │  │ WebRTC (aiortc)  │  │ API / WebSocket Relay  │ │
│  │ TCP :8554    │  │ UDP ICE 50001-…  │  │ TLS :443 (WS / REST)  │ │
│  └──────┬───────┘  └──────────────────┘  └───────────┬────────────┘ │
└─────────┼─────────────────────────────────────────────┼──────────────┘
          │ RTSP interleaved (TCP)                      │ WebSocket
          │ :8554                                       │ :443 /wss
          │                                             │
    ┌─────▼─────────────────────────────────────────────▼──────────┐
    │  Windows Host                                                 │
    │  ┌──────────────────────────┐  ┌────────────────────────────┐│
    │  │ Main Process             │  │ Worker Process              ││
    │  │ (SYSTEM, Windows Service)│  │ (User session)              ││
    │  │                          │  │                             ││
    │  │ • Service lifecycle      │  │ • WGC Capture (1 monitor)  ││
    │  │ • Registration/telemetry │  │ • x264 Encode (RTSP_ ref)  ││
    │  │ • Auto-update (60s)      │  │ • RTP Packetize (RFC 3984) ││
    │  │ • Spawn/kill worker      │  │ • RTSP Push → VPS :8554    ││
    │  │ • Inactivity monitoring  │  │ • WebSocket control input  ││
    │  │ • Agent WebSocket        │  │   (mouse, keyboard, clipb) ││
    │  └──────────┬───────────────┘  └────────────────────────────┘│
    │             │ IPC via ActivityShm (shared memory)             │
    │             └─────────────────────────────────────────────────┘
    └──────────────────────────────────────────────────────────────┘
```

### Транспорт

| Поток | Протокол | Порт VPS | Направление |
|-------|----------|----------|-------------|
| Видео | RTSP interleaved (TCP) | 8554 | Worker → VPS |
| Управление воркером (input) | WebSocket over TLS | 443 | Worker ↔ VPS |
| Управление агентом | WebSocket over TLS | 443 | Main ↔ VPS |
| Регистрация / телеметрия | HTTPS REST | 443 | Main → VPS |
| Auto-update | HTTPS download | 443 | Main → VPS |

---

## 2. Файловая структура RTSP_ (после интеграции)

```
/home/syadmin/project/RTSP_/
├── agent/                              # ← НОВОЕ: C++ агент
│   ├── cmd/agent/
│   │   ├── main.cpp                    # sysdm → порт: service + worker dispatch
│   │   ├── rdp_agent.h                 # sysdm → порт: RDPAgent class
│   │   ├── rdp_agent.cpp               # sysdm → порт: worker (input), но ingest заменён
│   │   ├── capture_wgc.h               # RTSP_ (эталон)
│   │   ├── capture_wgc.cpp             # RTSP_ (эталон)
│   │   ├── encoder_x264.h              # RTSP_ (эталон), только 1 monitor
│   │   ├── encoder_x264.cpp            # RTSP_ (эталон)
│   │   ├── rtsp_client.h               # RTSP_ → вынести RtspClient из main.cpp
│   │   ├── rtsp_client.cpp             # RTSP_ → вынести RtspClient из main.cpp
│   │   ├── h264_rtp_packetizer.h       # RTSP_ (эталон)
│   │   ├── h264_rtp_packetizer.cpp     # RTSP_ (эталон)
│   │   └── rtp_header.h                # RTSP_ (эталон)
│   ├── builder/
│   │   ├── Dockerfile.builder          # sysdm → порт (только x264 + WinRT)
│   │   ├── build_deps.sh               # sysdm → порт (только x264 + WinRT)
│   │   └── build_agents.py             # sysdm → порт (адаптировать файлы)
│   ├── build_deps_env/                 # Зависимости (libx264, WinRT headers)
│   ├── vendor/                         # (опционально) hiredis — скорее всего не нужно
│   └── sfx/                            # (опционально) 7z для самораспаковки
├── webrtc_server.py                    # Добавить API + WS relay
├── install.md                          # Обновить (Windows service setup)
├── scripts/                            # firewall и т.д.
└── ... (остальное без изменений)
```

**Файлы sysdm НЕ удаляются** — они остаются как пример, пока интеграция
не закончена. Удаляются только ссылки/упоминания.

---

## 3. Этапы реализации

### Этап 1: Build-система (Docker cross‑compilation)

Перенести из `sysdm` инфраструктуру сборки Windows-агента под Linux.

**sysdm → RTSP_ (копировать и адаптировать)**:

| sysdm | RTSP_ | Изменения |
|-------|-------|-----------|
| `Dockerfile.builder` | `agent/builder/Dockerfile.builder` | Убрать AMF/QSV/NVENC, оставить x264 + WinRT |
| `builder_cpp/build_deps.sh` | `agent/builder/build_deps.sh` | Только x264 + WinRT SDK, без GPU-кодеков |
| `builder_cpp/build_agents.py` | `agent/builder/build_agents.py` | Адаптировать под новую структуру `.cpp` файлов, без hiredis/Redis |
| `builder_cpp/build_deps_env/` | `agent/build_deps_env/` | Без изменений (после сборки) |
| `builder_cpp/sfx/` | `agent/sfx/` | Без изменений |
| `builder_cpp/install.cmd` | `agent/install.cmd` | Адаптировать под новое имя службы |
| `builder_cpp/uninstall.cmd` | `agent/uninstall.cmd` | Адаптировать под новое имя службы |

**Исключено** (не копировать):
- `vendor/hiredis-1.2.0/` — не нужен
- `build_deps.sh` секции AMF, oneVPL, NVENC
- Флаг `-DHAVE_REDIS` и `-l:hiredis.a`

**Что остаётся от sysdm**: файлы на месте, не трогать. Удаляются только ссылки
в коде RTSP_ на то, что не нужно.

---

### Этап 2: Service + two‑process architecture

Портировать из `sysdm/builder_cpp/agent/cmd/agent/main.cpp`:
- `installService()`, `uninstallService()`, `serviceMain()`, `serviceCtrlHandler()`
- `spawnRDPWorker()` → поиск активной сессии, `CreateProcessAsUserA`
- `ActivityShm` — shared memory для inactivity
- `mainLogic()` — регистрация, телеметрия, auto-update, WebSocket
- `checkForUpdate()` — HTTP + SHA256 + self‑replace
- Флаг `--rdp-worker` → точка входа воркера
- Флаг `--install` / `--uninstall`

**Не портировать из sysdm**:
- Весь ingest (HTTP chunked POST) — заменяется RTSP_ pipeline
- `redis_pubsub_thread()` — не нужен
- `disable_uac()`, `login_user()`, `create_admin_user()` — dashboard
- Команды: `process-list`, `kill-process`, `run-taskmgr` — dashboard
- Многомониторность — RTSP_ single monitor (только monitor 0)

---

### Этап 3: Worker — control channel + input handling

Портировать из `sysdm/builder_cpp/agent/cmd/agent/rdp_agent.cpp`:
- `control_loop()` — WebSocket приём команд
- `do_mouse_move()`, `do_mouse_move_and_click()`, `do_mouse_wheel()`
- `do_key()`, `do_text_input()`, `release_modifier_keys()`
- `cursor_watch_loop()` — детект курсора
- `clipboard_watch_loop()` — детект буфера обмена
- TLS + WebSocket клиент (Schannel)
- DWM keepalive

**Изменения относительно sysdm**:
- Удалить `encode_ingest_loop()` — заменить на RTSP_ pipeline
- Удалить многомониторный `PerMonitorState` — один capture, один encoder
- Input-код остаётся без изменений

---

### Этап 4: Worker — интеграция RTSP_ video pipeline

В worker-процессе заменить sysdm ingest на RTSP_ видео-пайплайн:

```
// Было (sysdm):
capture → encode → HTTP chunked POST

// Стало (RTSP_):
capture → encode → RTP packetize → RTSP client push (TCP :8554)
```

При этом:
- `capture_wgc.cpp` — RTSP_ версия (эталон), single monitor
- `encoder_x264.cpp` — RTSP_ версия (эталон), x264 конфиг из RTSP_
- `h264_rtp_packetizer.cpp` — RTSP_ версия (эталон)
- `rtsp_client.h/.cpp` — вынести inline-класс RtspClient из `main.cpp` в отдельный файл
- Сохранить `!K` keyframe request через `CheckForIncoming()`

---

### Этап 5: Server-side — API + WebSocket relay

Добавить в `webrtc_server.py`:

| Endpoint | Метод | Назначение |
|----------|-------|------------|
| `/api/agent/register` | POST | Регистрация агента |
| `/api/agent/telemetry` | POST | Телеметрия от агента |
| `/api/agent/check-update` | POST | Проверка новой версии |
| `/api/agent/builds` | GET/POST | CRUD билдов |
| `/relay/ws/control/agent/<uuid>` | WS | Команды агенту (start/stop worker) |
| `/relay/ws/control/worker/<agent_id>` | WS | Релей input команд браузера → worker |

Browser → Server → Agent relay:
```
Browser WebSocket → VPS WS endpoint → Внутренняя пересылка → Agent WS
```

---

### Этап 6: Сборка и тестирование

1. `docker compose build builder` — собрать контейнер для кросс-компиляции
2. `docker compose run --rm builder bash -c "bash agent/builder/build_deps.sh"` — зависимости
3. `docker compose run --rm builder python agent/builder/build_agents.py` — собрать `.exe`
4. Установить `.exe` на Windows через `install.cmd`
5. Проверить: служба запущена, воркер spawn, видео идёт, input работает
6. Проверить auto-update

---

## 4. Что не переносится

| Компонент | Причина |
|-----------|---------|
| Dashboard (веб-интерфейс sysdm) | Не нужен, RTSP_ самодостаточен |
| AMF / QSV / AVE / NVENC энкодеры | RTSP_ использует только x264 |
| Redis Pub/Sub | Не нужен для автономного модуля |
| DDA Capture | WGC — эталон в RTSP_ |
| HTTP chunked ingest | Заменён на RTSP interleaved |
| Многомониторность | RTSP_ single monitor |
| API управления ОС (create-user, disable-uac) | Специфика sysdm dashboard |
| Docker для VPS | RTSP_ использует systemd |

## 5. Примечания

- Файлы в `sysdm/` **не удаляются** — они остаются как reference до конца интеграции.
- При конфликте между кодом sysdm и RTSP_ приоритет у RTSP_ (эталон кодирования/транспорта).
- `main.cpp` из `CMakeLists.txt` RTSP_ остаётся как запасной вариант для локальной
  отладки (push mode). Основной билд — через `build_agents.py` с кросс-компиляцией.
