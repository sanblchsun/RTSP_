// ============================================================
// УСТАНОВКА DOCKER ENGINE + DOCKER COMPOSE (Mint/Ubuntu/Debian)
// ============================================================

# Удалить старый Docker
sudo apt remove docker docker-engine docker.io containerd runc

# Установить официальный репозиторий Docker
sudo apt update && sudo apt install -y ca-certificates curl
sudo install -m 0755 -d /etc/apt/keyrings
sudo curl -fsSL https://download.docker.com/linux/debian/gpg -o /etc/apt/keyrings/docker.asc
sudo chmod a+r /etc/apt/keyrings/docker.asc
echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.asc] https://download.docker.com/linux/debian $(. /etc/os-release && echo "${VERSION_CODENAME}") stable" | sudo tee /etc/apt/sources.list.d/docker.list > /dev/null

sudo apt remove docker-buildx
sudo apt install docker-buildx-plugin

sudo apt update && sudo apt install -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin


// ============================================================
// СБОРКА АГЕНТА (кросс-компиляция Windows x64)
// ============================================================

// Собрать контейнер builder (однократно)
docker compose --profile tools build builder

// Зависимости компиляции (однократно на новой машине)
docker compose --profile tools run --rm builder bash -c "rm -rf agent/builder/build_deps_env && bash agent/builder/build_deps.sh"

// Собрать агент (URL сервера берётся из .env SITE_DOMAIN)
docker compose --profile tools run --rm builder \
  python agent/builder/build_agents.py --slug 1.0.0

// Собрать setup-дистрибутив (после сборки агента)
docker compose --profile tools run --rm builder \
  python agent/builder/build_setup.py

// Интерактивный режим внутри контейнера
docker compose --profile tools run --rm builder bash


// ============================================================
// ЗАПУСК НА VPS (Debian, systemd)
// ============================================================

# читай файл install.md


// ============================================================
// РАЗВЁРТЫВАНИЕ ОБНОВЛЕНИЯ НА VPS
// ============================================================

# Остановить сервис
sudo systemctl stop rtsp-server

# Обновить файлы
git pull

# Обновить зависимости (если изменились)
pip install -r requirements.txt

# Запустить
sudo systemctl start rtsp-server


// ============================================================
// ПЕРЕСБОРКА / ОЧИСТКА BUILDER
// ============================================================

// Пересобрать builder (без кеша)
docker compose --profile tools build --no-cache builder


// ============================================================
// САМОПОДПИСАННЫЕ СЕРТИФИКАТЫ (dev)
// ============================================================

openssl req -x509 -newkey rsa:4096 -keyout key.pem -out cert.pem -days 365 -nodes -subj "/CN=localhost"


// ============================================================
// ЗАМЕТКИ
// ============================================================

// dist/agents/agent_1.0.0.exe — агент
// dist/agent_setup.exe — установщик (SFX 7z)
// build_deps_env собирается один раз, хранится в agent/builder/build_deps_env/
