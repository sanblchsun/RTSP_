#include "desktop_streamer.h"
#include <signal.h>
#include <iomanip>

std::unique_ptr<DesktopStreamer> g_streamer;

void signal_handler(int signal) {
    std::cout << "\nПолучен сигнал " << signal << ", останавливаем трансляцию..." << std::endl;
    if (g_streamer) {
        g_streamer->stop();
    }
}

int main(int argc, char* argv[]) {
    std::string rtsp_url = "udp://192.168.88.75:8554?pkt_size=1316";

    if (argc > 1) {
        rtsp_url = argv[1];
    }

    // Установка обработчика сигналов
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    std::cout << "=== Desktop Streamer ===" << std::endl;
    std::cout << "URL: " << rtsp_url << std::endl;
    std::cout << DesktopStreamer::get_system_info() << std::endl;

    try {
        g_streamer = std::make_unique<DesktopStreamer>(rtsp_url);

        // Настройка колбэков
        g_streamer->set_statistics_callback([](int64_t frames, double fps, double bitrate) {
            std::cout << "Статистика: " << frames << " фреймов, "
                << std::fixed << std::setprecision(1) << fps << " FPS, "
                << std::fixed << std::setprecision(1) << bitrate / 1000.0 << " Kbps\r" << std::flush;
            });

        g_streamer->set_error_callback([](const std::string& error, int code) {
            std::cerr << "\nОшибка: " << error;
            if (code != 0) {
                std::cerr << " (код: " << code << " - " << FFmpegError::to_string(code) << ")";
            }
            std::cerr << std::endl;
            });

        // Настройка параметров
        DesktopStreamer::CaptureSettings capture;
        capture.width = 1920;
        capture.height = 1080;
        capture.framerate = 30;
        g_streamer->set_capture_settings(capture);

        DesktopStreamer::EncodingSettings encoding;
        encoding.bitrate = 2000000; // 2 Mbps
        encoding.preset = "ultrafast";
        encoding.gop_size = 30;
        encoding.tune = "zerolatency";
        encoding.profile = "baseline";
        g_streamer->set_encoding_settings(encoding);

        if (!g_streamer->initialize()) {
            std::cerr << "Ошибка инициализации стримера" << std::endl;
            return -1;
        }

        std::cout << "Инициализация завершена успешно" << std::endl;
        std::cout << "Статус: " << DesktopStreamer::status_to_string(g_streamer->get_status()) << std::endl;

        // Запуск трансляции
        if (!g_streamer->stream()) {
            std::cerr << "Ошибка во время трансляции" << std::endl;
            return -1;
        }

    }
    catch (const std::exception& e) {
        std::cerr << "Исключение: " << e.what() << std::endl;
        return -1;
    }

    std::cout << "\nТрансляция завершена" << std::endl;
    return 0;
}