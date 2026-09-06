#include "provisions_websocket.h"
#include "provisions_endpoint_policy.h"

#include <esp_crt_bundle.h>
#include <esp_timer.h>
#include <esp_transport.h>
#include <esp_transport_ssl.h>
#include <esp_transport_ws.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>
#include <vector>

namespace {
constexpr size_t kMaxMessageBytes = 32768;
constexpr size_t kMaxBinaryBytes = 2048;
constexpr size_t kMaxPendingWrites = 4;
constexpr int kPollMs = 20;
constexpr int64_t kWriteUs = 3000000;
constexpr int64_t kConnectUs = 8000000;
constexpr int64_t kMessageUs = 5000000;
}  // namespace

struct ProvisionsWebSocket::State {
    struct Frame {
        std::vector<char> bytes;
        bool binary = false;
        int64_t deadline = 0;
        bool done = false;
        bool success = false;
    };
    std::atomic<bool> cancelled{false};
    std::atomic<bool> connected{false};
    std::atomic<int> error{0};
    std::mutex mutex;
    std::condition_variable changed;
    bool started = false;
    bool connect_done = false;
    std::string host, path, headers;
    std::function<void(const char*, size_t, bool)> on_data;
    std::function<void()> on_disconnected;
    std::deque<std::shared_ptr<Frame>> writes;
    // All handles and deadlines below belong exclusively to the I/O task.
    esp_transport_handle_t ssl = nullptr, parent = nullptr, websocket = nullptr;
    int64_t io_deadline = 0;

    int Remaining() const {
        if (cancelled.load())
            return 0;
        const auto remaining = io_deadline - esp_timer_get_time();
        return remaining <= 0
                   ? 0
                   : static_cast<int>(std::min<int64_t>((remaining + 999) / 1000, kPollMs));
    }
    static State& From(esp_transport_handle_t transport) {
        return *static_cast<State*>(esp_transport_get_context_data(transport));
    }
    static int Read(esp_transport_handle_t transport, char* data, int size, int) {
        auto& self = From(transport);
        while (const int wait = self.Remaining()) {
            const int read = esp_transport_read(self.ssl, data, size, wait);
            if (read > 0)
                return read;
            if (read != 0 && read != ESP_TLS_ERR_SSL_WANT_READ &&
                read != ESP_TLS_ERR_SSL_WANT_WRITE)
                return -1;
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        // A timeout after a frame/header has started cannot be mistaken for an
        // idle socket: its partially consumed frame requires reconnection.
        return -1;
    }
    static int Write(esp_transport_handle_t transport, const char* data, int size, int) {
        auto& self = From(transport);
        int sent = 0;
        while (sent < size) {
            const int wait = self.Remaining();
            if (!wait)
                return -1;
            const int written = esp_transport_write(self.ssl, data + sent, size - sent, wait);
            if (written > 0 && written <= size - sent)
                sent += written;
            else if (written != 0 && written != ESP_TLS_ERR_SSL_WANT_READ &&
                     written != ESP_TLS_ERR_SSL_WANT_WRITE)
                return -1;
            else
                vTaskDelay(pdMS_TO_TICKS(1));
        }
        return sent;
    }
    static int PollRead(esp_transport_handle_t transport, int requested) {
        auto& self = From(transport);
        const int wait = self.Remaining();
        return wait ? esp_transport_poll_read(self.ssl, std::min(wait, std::max(0, requested)))
                    : -1;
    }
    static int PollWrite(esp_transport_handle_t transport, int) {
        auto& self = From(transport);
        while (const int wait = self.Remaining()) {
            const int ready = esp_transport_poll_write(self.ssl, wait);
            if (ready != 0)
                return ready;
        }
        return -1;
    }
    bool Open() {
        ssl = esp_transport_ssl_init();
        parent = esp_transport_init();
        if (!ssl || !parent)
            return false;
        esp_transport_ssl_crt_bundle_attach(ssl, esp_crt_bundle_attach);
        // The async TLS API leaves its socket nonblocking. WANT_READ/WRITE are
        // handled by our finite loop, never by the vendor's unbounded sender.
        io_deadline = esp_timer_get_time() + kConnectUs;
        int result = 0;
        while (Remaining() && result == 0) {
            result = esp_transport_connect_async(ssl, host.c_str(), 443, 8000);
            if (result == 0)
                vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (result != 1 || !Remaining())
            return false;
        esp_transport_set_context_data(parent, this);
        esp_transport_set_func(
            parent,
            [](esp_transport_handle_t t, const char*, int, int) {
                return From(t).Remaining() ? 0 : -1;
            },
            Read, Write, [](esp_transport_handle_t) { return 0; }, PollRead, PollWrite,
            [](esp_transport_handle_t) { return 0; });
        websocket = esp_transport_ws_init(parent);
        if (!websocket)
            return false;
        esp_transport_ws_config_t config{};
        config.ws_path = path.c_str();
        config.headers = headers.c_str();
        config.propagate_control_frames = true;
        if (esp_transport_ws_set_config(websocket, &config) != ESP_OK)
            return false;
        // The same absolute deadline also bounds a slow-drip HTTP upgrade.
        return esp_transport_connect(websocket, host.c_str(), 443, 8000) == 0 &&
               esp_transport_ws_get_upgrade_request_status(websocket) == 101 && Remaining();
    }
    bool SendFrame(std::vector<char>& bytes, int opcode, int64_t deadline) {
        io_deadline = deadline;
        if (!Remaining())
            return false;
        return esp_transport_ws_send_raw(
                   websocket, static_cast<ws_transport_opcodes_t>(opcode | 0x80), bytes.data(),
                   static_cast<int>(bytes.size()), kPollMs) == static_cast<int>(bytes.size()) &&
               Remaining();
    }
    void Dispose() {
        if (websocket)
            esp_transport_destroy(websocket);
        if (parent)
            esp_transport_destroy(parent);
        if (ssl)
            esp_transport_destroy(ssl);
        websocket = parent = ssl = nullptr;
    }
    void Run() {
        const bool opened = Open();
        connected.store(opened && !cancelled.load());
        {
            std::lock_guard<std::mutex> lock(mutex);
            connect_done = true;
            changed.notify_all();
        }
        std::vector<char> message;
        message.reserve(kMaxMessageBytes);
        char chunk[kMaxBinaryBytes];
        bool message_open = false, binary = false, final = false;
        int frame_remaining = 0, opcode = 0;
        int64_t message_deadline = 0;
        while (connected.load() && !cancelled.load()) {
            std::shared_ptr<Frame> outgoing;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!writes.empty()) {
                    outgoing = std::move(writes.front());
                    writes.pop_front();
                }
            }
            if (outgoing) {
                const bool sent =
                    SendFrame(outgoing->bytes, outgoing->binary ? 2 : 1, outgoing->deadline);
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    outgoing->success = sent;
                    outgoing->done = true;
                    changed.notify_all();
                }
                if (!sent)
                    break;
            }
            if (message_open && esp_timer_get_time() >= message_deadline)
                break;
            io_deadline = esp_timer_get_time() + 500000;
            const int available = esp_transport_poll_read(websocket, kPollMs);
            if (available < 0)
                break;
            if (available == 0)
                continue;
            const int bytes = esp_transport_read(websocket, chunk, sizeof(chunk), kPollMs);
            if (bytes < 0)
                break;
            if (frame_remaining == 0) {
                frame_remaining = esp_transport_ws_get_read_payload_len(websocket);
                opcode = static_cast<int>(esp_transport_ws_get_read_opcode(websocket));
                final = esp_transport_ws_get_fin_flag(websocket);
                if (frame_remaining < 0 || frame_remaining > static_cast<int>(kMaxMessageBytes))
                    break;
                if (opcode < 8) {
                    if (opcode == 1 || opcode == 2) {
                        if (message_open)
                            break;
                        message_open = true;
                        binary = opcode == 2;
                        message_deadline = esp_timer_get_time() + kMessageUs;
                    } else if (opcode != 0 || !message_open)
                        break;
                }
            }
            if (bytes > frame_remaining || (bytes == 0 && frame_remaining > 0))
                break;
            frame_remaining -= bytes;
            if (opcode >= 8) {
                if (!final || frame_remaining != 0 || bytes > 125)
                    break;
                if (opcode == 8)
                    break;
                if (opcode == 9) {
                    std::vector<char> pong(chunk, chunk + bytes);
                    if (!SendFrame(pong, 10, esp_timer_get_time() + kWriteUs))
                        break;
                } else if (opcode != 10)
                    break;
                continue;
            }
            const size_t limit = binary ? kMaxBinaryBytes : kMaxMessageBytes;
            if (message.size() + bytes > limit)
                break;
            message.insert(message.end(), chunk, chunk + bytes);
            if (frame_remaining == 0 && final) {
                if (on_data && !cancelled.load())
                    on_data(message.data(), message.size(), binary);
                message.clear();
                message_open = false;
            }
        }
        const bool was_connected = connected.exchange(false);
        cancelled.store(true);
        error.store(-1);
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (auto& frame : writes)
                frame->done = true;
            writes.clear();
            changed.notify_all();
        }
        // This task is the sole TLS owner, including final cleanup. A destroyed
        // client or timed-out writer cannot free a handle still in use.
        Dispose();
        if (was_connected && on_disconnected)
            on_disconnected();
    }
};

ProvisionsWebSocket::ProvisionsWebSocket() : state_(std::make_shared<State>()) {}
ProvisionsWebSocket::~ProvisionsWebSocket() { Close(); }
void ProvisionsWebSocket::Close() {
    state_->cancelled.store(true);
    state_->changed.notify_all();
}
bool ProvisionsWebSocket::IsConnected() const {
    return state_->connected.load() && !state_->cancelled.load();
}
int ProvisionsWebSocket::GetLastError() const { return state_->error.load(); }
void ProvisionsWebSocket::SetHeader(const char* key, const char* value) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->started || !key || !value || std::strlen(key) > 64 || std::strlen(value) > 512 ||
        std::strpbrk(key, "\r\n:") || std::strpbrk(value, "\r\n")) {
        state_->cancelled.store(true);
        return;
    }
    state_->headers += std::string(key) + ": " + value + "\r\n";
    if (state_->headers.size() > 2048)
        state_->cancelled.store(true);
}
void ProvisionsWebSocket::OnData(std::function<void(const char*, size_t, bool)> callback) {
    state_->on_data = std::move(callback);
}
void ProvisionsWebSocket::OnDisconnected(std::function<void()> callback) {
    state_->on_disconnected = std::move(callback);
}
bool ProvisionsWebSocket::Connect(const char* uri) {
    if (!uri || std::string(uri) != ProvisionsEndpointPolicy::WebsocketUrl())
        return false;
    std::unique_lock<std::mutex> lock(state_->mutex);
    if (state_->started || state_->cancelled.load())
        return false;
    const std::string url(uri);
    const auto slash = url.find('/', 6);
    if (slash == std::string::npos)
        return false;
    state_->host = url.substr(6, slash - 6);
    if (state_->host.size() > 4 && state_->host.substr(state_->host.size() - 4) == ":443")
        state_->host.resize(state_->host.size() - 4);
    state_->path = url.substr(slash);
    state_->started = true;
    auto* owner = new (std::nothrow) std::shared_ptr<State>(state_);
    if (!owner || xTaskCreate(
                      [](void* argument) {
                          std::unique_ptr<std::shared_ptr<State>> owner(
                              static_cast<std::shared_ptr<State>*>(argument));
                          (*owner)->Run();
                          owner.reset();
                          vTaskDelete(nullptr);
                      },
                      "orbit_ws", 12288, owner, 3, nullptr) != pdPASS) {
        delete owner;
        state_->cancelled.store(true);
        return false;
    }
    if (!state_->changed.wait_for(lock, std::chrono::seconds(20), [&]() {
            return state_->connect_done || state_->cancelled.load();
        }))
        Close();
    return IsConnected();
}
bool ProvisionsWebSocket::Enqueue(const void* data, size_t size, bool binary, bool wait) {
    if (!data || size == 0 || size > (binary ? kMaxBinaryBytes : kMaxMessageBytes) ||
        !IsConnected())
        return false;
    auto frame = std::make_shared<State::Frame>();
    const auto* bytes = static_cast<const char*>(data);
    frame->bytes.assign(bytes, bytes + size);
    frame->binary = binary;
    frame->deadline = esp_timer_get_time() + kWriteUs;
    std::unique_lock<std::mutex> lock(state_->mutex);
    if (!IsConnected() || state_->writes.size() >= kMaxPendingWrites)
        return false;
    state_->writes.push_back(frame);
    if (!wait)
        return true;
    if (!state_->changed.wait_for(lock, std::chrono::milliseconds(3100),
                                  [&]() { return frame->done || state_->cancelled.load(); }))
        Close();
    return frame->done && frame->success;
}
bool ProvisionsWebSocket::Send(const std::string& text) {
    return Enqueue(text.data(), text.size(), false, true);
}
bool ProvisionsWebSocket::Send(const void* data, size_t size, bool binary) {
    return Enqueue(data, size, binary, true);
}
bool ProvisionsWebSocket::SendAsync(const std::string& text) {
    return Enqueue(text.data(), text.size(), false, false);
}
