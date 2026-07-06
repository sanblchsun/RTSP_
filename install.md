# Установка WebRTC Relay Server (Debian 13)

## 1. Копирование проекта

```bash
sudo mkdir -p /opt/webrtc-relay
sudo cp -r ~/RTSP_/* /opt/webrtc-relay/
sudo chown $USER:$USER /opt/webrtc-relay -R
```

## 2. Создание venv

```bash
cd /opt/webrtc-relay
sudo python3 -m venv venv
sudo venv/bin/pip install -r requirements.txt
```

## 3. Настройка

Создать `/opt/webrtc-relay/.env`:

```
SITE_DOMAIN=logovoprog.ru
SSL_CERT_PATH=/opt/webrtc-relay/letsencrypt/live/logovoprog.ru/fullchain.pem
SSL_KEY_PATH=/opt/webrtc-relay/letsencrypt/live/logovoprog.ru/privkey.pem
f087827e12c51bba377ef81d09c55e74acdca54b```

## 4. Systemd-сервис

Создать `/etc/systemd/system/webrtc-relay.service`:

```ini
[Unit]
Description=WebRTC relay server
After=network.target

[Service]
Type=simple
WorkingDirectory=/opt/webrtc-relay
Environment=PATH=/opt/webrtc-relay/venv/bin
EnvironmentFile=/opt/webrtc-relay/.env
ExecStart=/opt/webrtc-relay/venv/bin/python webrtc_server.py
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now webrtc-relay
```

## 5. Firewall

Открыть только TCP порты (22, 8001, 8554) и UDP для WebRTC ICE:

```bash
sudo ./scripts/allowlist_firewall.sh <IP_агента>
```

## 6. Логи

```bash
sudo journalctl -u webrtc-relay -f
sudo systemctl restart webrtc-relay && sudo journalctl -u webrtc-relay -f --since=now
