# Установка WebRTC Relay Server (Debian 13)

## 1. Копирование проекта

```bash
sudo mkdir -p /opt/webrtc-relay
cd /opt
sudo clone <репозиторий> webrtc-relay
sudo chown "$USER":"$USER" /opt/webrtc-relay -R
```

## 2. Создание venv

```bash
cd /opt/webrtc-relay
python3 -m venv venv
venv/bin/pip install -r requirements.txt
```

## 3. Настройка

Создать `/opt/webrtc-relay/.env`:

```
SITE_DOMAIN=<domain.ru>
SSL_CERT_PATH=/opt/webrtc-relay/letsencrypt/live/<domain.ru>/fullchain.pem
SSL_KEY_PATH=/opt/webrtc-relay/letsencrypt/live/<domain.ru>/privkey.pem
```

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
````

## 5. Firewall

Открыть только TCP порты (22, 8002, 8555) и UDP для WebRTC ICE:

```bash
sudo ./scripts/allowlist_firewall.sh <IP_агента>
```

## 6. Логи

```bash
sudo journalctl -u webrtc-relay -f
sudo systemctl restart webrtc-relay && sudo journalctl -u webrtc-relay -f --since=now
