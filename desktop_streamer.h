#pragma once
#ifndef DESKTOP_STREAMER_H
#define DESKTOP_STREAMER_H

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <iostream>
#include <thread>
#include <chrono>
#include <string>
#include <cstdlib>
#include <memory>
#include <functional>
#include <atomic>
#include <mutex>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/time.h>
#include <libavutil/opt.h>
#include <libavdevice/avdevice.h>
#include <libswscale/swscale.h>
}

/**
 * @brief Класс для захвата рабочего стола и трансляции через RTSP
 */
class DesktopStreamer {
public:
    /**
     * @brief Структура для настроек захвата
     */
    struct CaptureSettings {
        int width = 1920;           ///< Ширина захватываемого видео
        int height = 1080;          ///< Высота захватываемого видео
        int framerate = 30;         ///< Частота кадров
        int offset_x = 0;           ///< Смещение по X (для захвата части экрана)
        int offset_y = 0;           ///< Смещение по Y (для захвата части экрана)
        std::string device_name;    ///< Имя устройства (опционально)
    };

    /**
     * @brief Структура для настроек кодирования
     */
    struct EncodingSettings {
        int bitrate = 2000000;      ///< Битрейт в bps (по умолчанию 2 Mbps)
        int gop_size = 30;          ///< Размер GOP (группы картинок)
        int max_b_frames = 0;       ///< Максимальное количество B-фреймов
        std::string preset = "ultrafast";  ///< Пресет для H.264 энкодера
        std::string tune = "zerolatency";   ///< Настройка для минимальной задержки
        std::string profile = "baseline";   ///< Профиль H.264
        AVPixelFormat pixel_format = AV_PIX_FMT_YUV420P;  ///< Формат пикселей
    };

    /**
     * @brief Перечисление статусов стримера
     */
    enum class StreamerStatus {
        UNINITIALIZED,  ///< Не инициализирован
        INITIALIZED,    ///< Инициализирован
        STREAMING,      ///< Выполняется трансляция
        STOPPED,        ///< Остановлен
        ERROR_STATE     ///< Ошибка
    };

    /**
     * @brief Тип функции обратного вызова для обработки статистики
     * @param frames_processed Количество обработанных фреймов
     * @param fps Текущий FPS
     * @param bitrate Текущий битрейт
     */
    using StatisticsCallback = std::function<void(int64_t frames_processed, double fps, double bitrate)>;

    /**
     * @brief Тип функции обратного вызова для обработки ошибок
     * @param error_message Сообщение об ошибке
     * @param error_code Код ошибки FFmpeg (если применимо)
     */
    using ErrorCallback = std::function<void(const std::string& error_message, int error_code)>;

private:
    // Контексты FFmpeg
    AVFormatContext* input_ctx_;
    AVFormatContext* output_ctx_;
    AVCodecContext* decoder_ctx_;
    AVCodecContext* encoder_ctx_;
    SwsContext* sws_ctx_;

    // Настройки
    std::string rtsp_url_;
    CaptureSettings capture_settings_;
    EncodingSettings encoding_settings_;

    // Состояние
    StreamerStatus status_;
    int video_stream_index_;
    std::atomic<bool> should_stop_;
    std::thread streaming_thread_;

    // Статистика
    int64_t frames_processed_;
    std::chrono::high_resolution_clock::time_point start_time_;
    std::chrono::high_resolution_clock::time_point last_stats_time_;

    // Колбэки
    StatisticsCallback stats_callback_;
    ErrorCallback error_callback_;

public:
    /**
     * @brief Конструктор
     * @param rtsp_server_url URL RTSP сервера
     */
    explicit DesktopStreamer(const std::string& rtsp_server_url);

    /**
     * @brief Деструктор
     */
    ~DesktopStreamer();

    // Запрещаем копирование и присваивание
    DesktopStreamer(const DesktopStreamer&) = delete;
    DesktopStreamer& operator=(const DesktopStreamer&) = delete;

    // Разрешаем перемещение
    DesktopStreamer(DesktopStreamer&& other) noexcept;
    DesktopStreamer& operator=(DesktopStreamer&& other) noexcept;

    /**
     * @brief Инициализация стримера
     * @return true в случае успеха, false при ошибке
     */
    bool initialize();

    /**
     * @brief Запуск трансляции (блокирующий вызов)
     * @return true в случае успеха, false при ошибке
     */
    bool stream();

    /**
     * @brief Запуск трансляции в отдельном потоке
     * @return true в случае успеха, false при ошибке
     */
    bool start_async();

    /**
     * @brief Остановка трансляции
     */
    void stop();

    /**
     * @brief Ожидание завершения трансляции (если запущена асинхронно)
     */
    void wait_for_completion();

    /**
     * @brief Получение текущего статуса
     * @return Текущий статус стримера
     */
    StreamerStatus get_status() const;

    /**
     * @brief Получение строкового представления статуса
     * @param status Статус для преобразования
     * @return Строковое представление статуса
     */
    static std::string status_to_string(StreamerStatus status);

    // Методы для настройки параметров

    /**
     * @brief Установка настроек захвата
     * @param settings Настройки захвата
     */
    void set_capture_settings(const CaptureSettings& settings);

    /**
     * @brief Установка настроек кодирования
     * @param settings Настройки кодирования
     */
    void set_encoding_settings(const EncodingSettings& settings);

    /**
     * @brief Получение настроек захвата
     * @return Текущие настройки захвата
     */
    const CaptureSettings& get_capture_settings() const;

    /**
     * @brief Получение настроек кодирования
     * @return Текущие настройки кодирования
     */
    const EncodingSettings& get_encoding_settings() const;

    /**
     * @brief Установка колбэка для статистики
     * @param callback Функция обратного вызова
     */
    void set_statistics_callback(const StatisticsCallback& callback);

    /**
     * @brief Установка колбэка для ошибок
     * @param callback Функция обратного вызова
     */
    void set_error_callback(const ErrorCallback& callback);

    /**
     * @brief Получение статистики
     * @param frames_processed Выходной параметр - количество обработанных фреймов
     * @param fps Выходной параметр - текущий FPS
     * @param duration_seconds Выходной параметр - длительность трансляции в секундах
     */
    void get_statistics(int64_t& frames_processed, double& fps, double& duration_seconds) const;

    // Статические методы

    /**
     * @brief Инициализация библиотеки FFmpeg (вызывается автоматически)
     */
    static void initialize_ffmpeg();

    /**
     * @brief Получение списка доступных устройств захвата
     * @return Вектор имен доступных устройств
     */
    static std::vector<std::string> get_available_capture_devices();

    /**
     * @brief Получение информации о системе
     * @return Строка с информацией о FFmpeg и системе
     */
    static std::string get_system_info();

    /**
     * @brief Проверка поддержки RTSP
     * @return true если RTSP поддерживается
     */
    static bool is_rtsp_supported();

private:
    /**
     * @brief Настройка входного потока (захват рабочего стола)
     * @return true в случае успеха
     */
    bool setup_input();

    /**
     * @brief Настройка выходного потока (RTSP)
     * @return true в случае успеха
     */
    bool setup_output();

    /**
     * @brief Основной цикл трансляции
     */
    void streaming_loop();

    /**
     * @brief Переподключение выходного потока (при обрыве клиента)
     * @return true в случае успеха
     */
    bool reconnect_output();

    /**
     * @brief Очистка ресурсов
     */
    void cleanup();

    /**
     * @brief Обработка ошибки
     * @param message Сообщение об ошибке
     * @param ffmpeg_error Код ошибки FFmpeg (опционально)
     */
    void handle_error(const std::string& message, int ffmpeg_error = 0);

    /**
     * @brief Обновление статистики
     */
    void update_statistics();

    /**
     * @brief Перемещение ресурсов из другого объекта
     * @param other Исходный объект
     */
    void move_from(DesktopStreamer&& other) noexcept;
};

/**
 * @brief Утилитный класс для работы с FFmpeg ошибками
 */
class FFmpegError {
public:
    /**
     * @brief Преобразование кода ошибки FFmpeg в строку
     * @param error_code Код ошибки
     * @return Описание ошибки
     */
    static std::string to_string(int error_code);

    /**
     * @brief Проверка, является ли код ошибкой EOF
     * @param error_code Код ошибки
     * @return true если это EOF
     */
    static bool is_eof(int error_code);
};

/**
 * @brief Утилитный класс для работы с RTSP URL
 */
class RTSPUtils {
public:
    /**
     * @brief Валидация RTSP URL
     * @param url URL для проверки
     * @return true если URL валидный
     */
    static bool validate_url(const std::string& url);

    /**
     * @brief Извлечение хоста из RTSP URL
     * @param url RTSP URL
     * @return Хост или пустая строка при ошибке
     */
    static std::string extract_host(const std::string& url);

    /**
     * @brief Извлечение порта из RTSP URL
     * @param url RTSP URL
     * @return Порт или 554 (по умолчанию для RTSP)
     */
    static int extract_port(const std::string& url);

    /**
     * @brief Извлечение пути из RTSP URL
     * @param url RTSP URL
     * @return Путь или пустая строка
     */
    static std::string extract_path(const std::string& url);
};

#endif // DESKTOP_STREAMER_H