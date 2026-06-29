// builder_cpp/agent/cmd/agent/rdp_agent.h
#ifndef RDP_AGENT_H
#define RDP_AGENT_H

#define WIN32_LEAN_AND_MEAN
#define SECURITY_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <security.h>
#include <schannel.h>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <memory>
#include <chrono>
#include <condition_variable>

#include "capture_base.h"
#include "encoder.h"

// ============ LOGGING (реализуется в main.cpp) ============
extern void log(const char *msg);
extern void log(const std::string &msg);
extern void logf(const char *fmt, ...);
extern bool disable_uac();  // UAC control (реализуется в main.cpp)

// ============ INTERNAL STRUCTURES ============

struct MonitorInfo
{
    int x = 0, y = 0, w = 0, h = 0;
};

// Per-monitor state for capture → encode → ingest pipeline
struct PerMonitorState
{
    int id = 0;
    int w = 0, h = 0;
    int origin_x = 0, origin_y = 0;
    int fps = 5;
    int quality_qp = 23;
    int measured_bitrate_kbps = 0;
    std::string encoder_type = "cpu";
    std::unique_ptr<IEncoder> encoder;

    std::mutex m;
    std::condition_variable cv;
    std::vector<uint8_t> latest_frame;
    bool has_new = false;
    bool stop = false;

    // Tracking
    uint64_t frame_count = 0;
    uint64_t bytes_total = 0;
    int ingest_frames_5s = 0;
    uint64_t ingest_bytes_5s = 0;
    int64_t avg_encode_us = 0; // running average of EncodeFrame duration
    std::chrono::steady_clock::time_point t0;

    // Auto-adjust ramp
    int auto_adj_fps = 5;
    std::chrono::steady_clock::time_point auto_adj_next;
};

// Shared memory between main process (SYSTEM) and RDP worker (user session)
struct ActivityShm
{
    volatile LONG64 last_activity_time; // written by worker on input events
    volatile LONG timeout_min;          // written by worker from config poll
};

struct RDPConfig
{
    std::string server_host = "127.0.0.1";
    int server_port = 443;
    std::string agent_id = "agent1";
    std::string agent_token = "";
    bool verify_cert = true;

    std::string codec;
    std::string encoder;
    std::string quality_preset = "medium";
    int framerate = 30;

    int timeout_min = 30;
    std::string shm_name; // shared memory name for activity tracking
};

struct RDPRuntime
{
    std::mutex m;
    std::string codec, encoder;
    std::string quality_preset = "medium";
    int quality_qp = 23;
    int framerate = 30;
    std::atomic<bool> restart{false};
    std::atomic<bool> stop{false};

    std::mutex ctrl_sock_m;
    struct TlsConn *ctrl_conn = nullptr;

    // Manual mode: when true, fps and quality come from config (user-set).
    // When false, auto mode: start at 5fps, ramp up every 30s.
    bool manual_mode = false;

    // Auto-adjust ramp (auto mode): start low, climb every ~30s
    int auto_adj_fps = 5;
    std::chrono::steady_clock::time_point auto_adj_next;

    // Inactivity timeout tracking (shared with parent process)
    int timeout_min = 30;
    std::chrono::steady_clock::time_point last_activity_time;
};

struct TlsConn
{
    SOCKET sock = INVALID_SOCKET;
    CredHandle cred = {};
    CtxtHandle ctx = {};
    bool cred_ok = false;
    bool ctx_ok = false;
    SecPkgContext_StreamSizes sizes = {};
    std::vector<uint8_t> raw;
    std::vector<uint8_t> plain;
    std::mutex send_m;
};

// ============ RDP AGENT CLASS ============

class RDPAgent
{
public:
    RDPAgent(const RDPConfig &config);
    ~RDPAgent();

    void start();
    void stop();
    bool isRunning() const;

    struct Status
    {
        int screen_w = 1920;
        int screen_h = 1080;
        bool is_connected = false;
        std::string last_error;
        int h264_frames = 0;
    };
    Status getStatus() const;

    // Public API used by main.cpp control command loop
    static TlsConn *tls_connect(const std::string &host, int port, bool verify_cert);
    static void tls_close(TlsConn *c);
    static bool ws_handshake(TlsConn *c, const std::string &host, int port, const std::string &path);
    static int ws_recv(TlsConn *c, std::vector<uint8_t> &payload);
    static bool ws_send(TlsConn *c, int op, const void *data, size_t len);
    static bool json_str(const std::string &j, const std::string &k, std::string &out);
    static bool json_str_ex(const std::string &j, const std::string &k, std::string &out);
    static bool json_int(const std::string &j, const std::string &k, int &out);
    static bool sock_has_data(TlsConn *c, int timeout_ms);

private:
    RDPConfig config;
    RDPRuntime runtime;
    std::string last_config_sig;
    std::vector<std::thread> threads;
    bool running = false;

    // Shared memory for activity tracking (written by worker, read by main process)
    HANDLE shm_handle = nullptr;
    ActivityShm *shm = nullptr;

    // TLS (internal)
    static bool tls_handshake(TlsConn *c, const std::string &host, bool verify_cert);
    static bool tls_send_all(TlsConn *c, const char *p, int n);
    static int tls_recv_some(TlsConn *c, char *buf, int want);
    static int tls_recv_n(TlsConn *c, char *p, int n);

    // Raw TCP
    static bool send_all_raw(SOCKET s, const char *p, int n);
    static int recv_n_raw(SOCKET s, char *p, int n);
    static SOCKET tcp_connect(const std::string &host, int port);

    // WebSocket (internal)
    static std::string b64(const unsigned char *d, size_t n);

public:
    // JSON
    static std::string json_escape(const std::string &s);

    // Input
    static void do_mouse_move(int x, int y, int monitor_id = 0);
    static void do_mouse_button(int button, bool down);
    static void do_mouse_wheel(int delta);
    static void do_mouse_move_and_click(int x, int y, int button, bool down, int monitor_id = 0);
    static void do_text_input(const std::string &utf8);
    static int code_to_vk(const std::string &code);
    static void do_key(const std::string &code, bool down);
    static void run_taskmgr();
    static std::string clipboard_read_utf8();
    static void clipboard_write_utf8(const std::string &utf8);
    void handle_control(const std::string &j);

    // Coordinate conversion helper
    static bool mouse_to_abs(int x, int y, int monitor_id, LONG &dx, LONG &dy, int sw, int sh);

    // Screen metrics
    static bool read_screen_metrics(int &w, int &h, int &ox, int &oy);
    static void init_screen_metrics();
    static void release_modifier_keys();
    static bool is_secure_desktop_active();
    static bool is_consent_exe_running();

    // Control
    void ctrl_send_hello();
    void ctrl_send_clipboard(const std::string &text);
    void ctrl_send_cursor(int shape_id);
    void ctrl_send_monitor_status(int monitor_id, int bitrate_kbps, int fps, bool overloaded = false);
    static std::string make_hello_json();
    static int preset_to_qp(const std::string &preset);

    // Loops
    void control_loop();
    void resolution_watch_loop();
    void clipboard_watch_loop();
    void cursor_watch_loop();
    void session_keepalive_loop();
    static void wake_dwm_now();

    // Per-monitor capture → encode → ingest pipeline
    bool init_capture();
    void shutdown_capture();
    void capture_loop();
    void encode_ingest_loop(int monitor_id);
    std::unique_ptr<CaptureBase> m_capture;
    std::vector<std::unique_ptr<PerMonitorState>> m_monitors;
    std::vector<std::thread> m_ingest_threads;

    // Globals
    static std::atomic<int> g_screen_w;
    static std::atomic<int> g_screen_h;
    static std::atomic<int> g_video_w;
    static std::atomic<int> g_video_h;
    static std::atomic<int> g_screen_origin_x;
    static std::atomic<int> g_screen_origin_y;
    static std::atomic<int64_t> g_last_frame_time;
    static std::mutex g_clip_m;
    static std::string g_last_clip;

    // Cached virtual screen metrics (refreshed on resolution change)
    static std::atomic<int> g_vscreen_x;
    static std::atomic<int> g_vscreen_y;
    static std::atomic<int> g_vscreen_w;
    static std::atomic<int> g_vscreen_h;
    static void refresh_vscreen_cache();

    // Last mouse position for redundant-move skip
    static std::atomic<int> g_last_mouse_x;
    static std::atomic<int> g_last_mouse_y;
    static std::atomic<bool> g_input_pending;

    // Monitor list (informational, sent in hello)
    static std::mutex g_monitors_m;
    static std::vector<MonitorInfo> g_monitors;
    static void read_monitors(std::vector<MonitorInfo> &out);
};

// ============ USER-SESSION WORKER ENTRYPOINT ============
// Вызывается из main.cpp при флаге --rdp-worker.
// Блокирует поток до завершения процесса (TerminateProcess извне).
int run_rdp_worker(const std::string &host, int port,
                   const std::string &agent_id,
                   const std::string &agent_token,
                   bool verify_cert,
                   int timeout_min = 30,
                   const std::string &shm_name = "",
                   const std::string &codec = "",
                   const std::string &encoder = "",
                   const std::string &quality_preset = "",
                   int fps = 0);

#endif // RDP_AGENT_H
