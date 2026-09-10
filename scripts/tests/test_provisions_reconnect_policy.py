"""Host checks for bounded Provisions gateway reconnect scheduling."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def method(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def run_cpp(program: str) -> None:
    compiler = shutil.which("c++")
    if compiler is None:
        raise unittest.SkipTest("c++ compiler unavailable")
    with tempfile.TemporaryDirectory(prefix="orbit-reconnect-policy-") as directory:
        source = Path(directory) / "policy.cc"
        binary = Path(directory) / "policy"
        source.write_text(program, encoding="utf-8")
        built = subprocess.run(
            [
                compiler,
                "-std=c++17",
                "-fsanitize=address,undefined",
                "-I",
                str(ROOT / "main"),
                str(source),
                "-o",
                str(binary),
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        if built.returncode != 0:
            raise AssertionError(built.stderr)
        result = subprocess.run(
            [str(binary)], capture_output=True, text=True, timeout=10, check=False
        )
        if result.returncode != 0:
            raise AssertionError(result.stdout + result.stderr)


class ProvisionsReconnectPolicyTests(unittest.TestCase):
    def test_policy_is_bounded_saturating_and_never_zero(self) -> None:
        run_cpp(
            r"""
#include <cassert>
#include <limits>
#include "provisions_reconnect_policy.h"

int main() {
    using namespace provisions::reconnect;
    constexpr int bases[] = {1, 2, 4, 8, 16, 16};
    for (int attempt = 0; attempt <= 5; ++attempt) {
        for (uint32_t jitter = 0; jitter < kJitterSpanTicks; ++jitter)
            assert(DelayTicks(attempt, jitter) == bases[attempt] + static_cast<int>(jitter));
    }
    assert(DelayTicks(std::numeric_limits<int>::max(), 7) == 19);
    assert(WorkerFailureDelayTicks(0) == 16);
    assert(WorkerFailureDelayTicks(3) == 19);

    auto fifth = AfterGatewayFailure(4, 0);
    assert(fifth.attempts == 5 && fifth.wait_ticks == 16);
    auto sixth = AfterGatewayFailure(fifth.attempts, 1);
    assert(sixth.attempts == 6 && sixth.wait_ticks == 17);
    auto saturated = AfterGatewayFailure(std::numeric_limits<int>::max(), 2);
    assert(saturated.attempts == std::numeric_limits<int>::max());
    assert(saturated.wait_ticks == 18);
}
"""
        )

    def test_actual_maintenance_recovers_after_five_but_preserves_pending_rollback(self) -> None:
        application = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        handler = method(application, "void Application::HandleProvisionsGatewayMaintenance()")
        run_cpp(
            r"""
#include <atomic>
#include <cassert>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include "provisions_reconnect_policy.h"

#define CONFIG_PROVISIONS_LOCAL_CAPTURE 0
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
constexpr int kProvisionsHeartbeatIntervalSeconds = 15;
constexpr int kProvisionsResponseTimeoutSeconds = 30;
constexpr int kProvisionsMaximumReconnectAttempts = 5;
constexpr int kDeviceStateIdle = 0;
constexpr int kDeviceStateSpeaking = 2;
enum class PowerSaveLevel { LOW_POWER };
namespace Lang { namespace Sounds { constexpr std::string_view OGG_EXCLAMATION = "error"; } }

uint32_t entropy_value = 0;
int restarts = 0;
int64_t esp_timer_get_time() { return 0; }
uint32_t esp_random() { return entropy_value++; }
void esp_restart() { ++restarts; }

struct Display {
    std::string status;
    void SetStatus(const char* value) { status = value; }
};
struct Board {
    Display display;
    static Board& GetInstance() { static Board board; return board; }
    Display* GetDisplay() { return &display; }
    void SetPowerSaveLevel(PowerSaveLevel) {}
};
struct AudioService { void ResetDecoder() {} };
struct Protocol {
    virtual ~Protocol() = default;
    bool open = false;
    int open_calls = 0;
    std::deque<bool> results;
    bool IsAudioChannelOpened() const { return open; }
    bool OpenAudioChannel() {
        ++open_calls;
        const bool result = results.empty() ? false : results.front();
        if (!results.empty()) results.pop_front();
        open = result;
        return result;
    }
    void CloseAudioChannel() { open = false; }
};
struct WebsocketProtocol : Protocol {
    bool IsGatewayHeartbeatExpired() const { return false; }
    bool SendGatewayHeartbeat() { return true; }
};
struct Ota {
    bool pending = false;
    bool marked = false;
    bool IsCurrentVersionPendingVerification() const { return pending; }
    bool HasServerTime() const { return true; }
    void MarkCurrentVersionValid() { marked = true; }
};
struct Application {
    std::shared_ptr<WebsocketProtocol> protocol = std::make_shared<WebsocketProtocol>();
    std::unique_ptr<Ota> ota_;
    std::atomic<bool> provisions_response_pending_{false};
    std::atomic<bool> network_connected_{true};
    std::atomic<int64_t> provisions_tts_deadline_us_{0};
    int provisions_response_ticks_ = 0;
    int provisions_heartbeat_ticks_ = 0;
    int provisions_reconnect_attempts_ = 0;
    int provisions_reconnect_wait_ticks_ = 0;
    void* activation_task_handle_ = nullptr;
    bool aborted_ = false;
    bool has_server_time_ = false;
    int state = kDeviceStateIdle;
    AudioService audio_service_;

    std::shared_ptr<Protocol> GetProtocol() { return protocol; }
    int GetDeviceState() const { return state; }
    void SetDeviceState(int value) { state = value; }
    void SetProvisionsResponsePending(bool value) { provisions_response_pending_ = value; }
    void InvalidateProvisionsTtsTurn() {}
    const char* GetProvisionsIdleStatus() const { return "Ready"; }
    void Alert(const char*, const char*, const char*, std::string_view) {}
    void HandleProvisionsGatewayMaintenance();
};

__HANDLER__

void run_until(Application& app, int expected_open_calls) {
    for (int tick = 0; tick < 1000 && app.protocol->open_calls < expected_open_calls; ++tick)
        app.HandleProvisionsGatewayMaintenance();
}

int main() {
    Application confirmed;
    confirmed.protocol->results = {false, false, false, false, false, false, false, true};
    run_until(confirmed, 8);
    assert(confirmed.protocol->open);
    assert(confirmed.protocol->open_calls == 8);
    assert(confirmed.provisions_reconnect_attempts_ == 0);
    assert(confirmed.provisions_reconnect_wait_ticks_ == 0);
    assert(restarts == 0);

    Application pending;
    pending.ota_ = std::make_unique<Ota>();
    pending.ota_->pending = true;
    pending.protocol->results = {false, false, false, false, false, true};
    run_until(pending, 5);
    while (restarts == 0) pending.HandleProvisionsGatewayMaintenance();
    assert(pending.protocol->open_calls == 5);
    assert(pending.provisions_reconnect_attempts_ == 5);
    assert(restarts == 1);

    Application offline;
    offline.network_connected_ = false;
    for (int tick = 0; tick < 20; ++tick) offline.HandleProvisionsGatewayMaintenance();
    assert(offline.protocol->open_calls == 0);

    Application busy_state;
    busy_state.state = 1;
    for (int tick = 0; tick < 20; ++tick) busy_state.HandleProvisionsGatewayMaintenance();
    assert(busy_state.protocol->open_calls == 0);
}
""".replace("__HANDLER__", handler)
        )

    def test_async_worker_and_local_capture_gates_remain_bounded(self) -> None:
        application = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        maintenance = method(application, "void Application::HandleProvisionsGatewayMaintenance()")
        reconnect = method(application, "void Application::ReconnectVoiceGateway()")

        self.assertIn("if (manual_listening_requested_.load())", maintenance)
        self.assertIn("if (provisions_network_busy_.load())", maintenance)
        self.assertIn("!network_connected_.load()", maintenance)
        self.assertIn("compare_exchange_strong(expected, true)", reconnect)
        self.assertIn("opened && app->network_connected_.load()", reconnect)
        self.assertIn("MarkCurrentVersionValid()", reconnect)
        self.assertIn("WorkerFailureDelayTicks(esp_random())", reconnect)
        self.assertIn("AfterGatewayFailure(", reconnect)
        self.assertNotIn("provisions_recorder_.reset", maintenance + reconnect)
        self.assertIn("kProvisionsMaximumReconnectAttempts = 5", application)
        self.assertIn("ota_->IsCurrentVersionPendingVerification()", maintenance)

    def test_actual_async_reconnect_is_single_flight_and_cleans_up_failures(self) -> None:
        application = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        reconnect = method(application, "void Application::ReconnectVoiceGateway()")
        run_cpp(
            r"""
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <vector>
#include "provisions_reconnect_policy.h"

#define pdPASS 1
constexpr int kDeviceStateIdle = 0;
enum class PowerSaveLevel { LOW_POWER };

bool fail_nothrow_allocation = false;
int task_create_result = pdPASS;
int task_create_calls = 0;
uint32_t entropy_value = 0;
std::function<void()> pending_task;

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return fail_nothrow_allocation ? nullptr : std::malloc(size);
}
void operator delete(void* pointer, const std::nothrow_t&) noexcept { std::free(pointer); }
uint32_t esp_random() { return entropy_value++; }
void vTaskDelete(void*) {}
int xTaskCreate(void (*task)(void*), const char*, int, void* argument, int, void*) {
    ++task_create_calls;
    if (task_create_result != pdPASS) return task_create_result;
    pending_task = [task, argument]() { task(argument); };
    return pdPASS;
}

struct Display {
    std::string status;
    void SetStatus(const char* value) { status = value; }
};
struct Board {
    Display display;
    static Board& GetInstance() { static Board board; return board; }
    Display* GetDisplay() { return &display; }
};
struct Protocol {
    bool open_result = false;
    int opens = 0;
    int closes = 0;
    bool OpenAudioChannel() { ++opens; return open_result; }
    void CloseAudioChannel() { ++closes; }
};
struct Ota {
    bool marked = false;
    bool HasServerTime() const { return true; }
    void MarkCurrentVersionValid() { marked = true; }
};
struct Application {
    std::shared_ptr<Protocol> protocol = std::make_shared<Protocol>();
    std::unique_ptr<Ota> ota_;
    std::atomic<bool> provisions_network_busy_{false};
    std::atomic<bool> network_connected_{true};
    int provisions_reconnect_attempts_ = 0;
    int provisions_reconnect_wait_ticks_ = 0;
    bool has_server_time_ = false;
    int state = kDeviceStateIdle;
    std::deque<std::function<void()>> scheduled;

    std::shared_ptr<Protocol> GetProtocol() { return protocol; }
    int GetDeviceState() const { return state; }
    const char* GetProvisionsIdleStatus() const { return "Ready"; }
    void Schedule(std::function<void()> work) { scheduled.push_back(std::move(work)); }
    void Drain() {
        while (!scheduled.empty()) {
            auto work = std::move(scheduled.front());
            scheduled.pop_front();
            work();
        }
    }
    void ReconnectVoiceGateway();
};

__RECONNECT__

void RunWorker(Application& app) {
    assert(pending_task);
    auto task = std::move(pending_task);
    pending_task = {};
    task();
    app.Drain();
}

int main() {
    Application single;
    single.ReconnectVoiceGateway();
    assert(single.provisions_network_busy_ && task_create_calls == 1);
    single.ReconnectVoiceGateway();
    assert(task_create_calls == 1);
    RunWorker(single);
    assert(!single.provisions_network_busy_);
    assert(single.provisions_reconnect_attempts_ == 1);
    assert(single.provisions_reconnect_wait_ticks_ >= 1 &&
           single.provisions_reconnect_wait_ticks_ <= 4);
    assert(single.protocol->closes == 1);

    Application extended;
    for (int failure = 0; failure < 7; ++failure) {
        extended.ReconnectVoiceGateway();
        RunWorker(extended);
    }
    assert(extended.provisions_reconnect_attempts_ == 7);
    assert(extended.provisions_reconnect_wait_ticks_ >= 16 &&
           extended.provisions_reconnect_wait_ticks_ <= 19);
    extended.protocol->open_result = true;
    extended.ota_ = std::make_unique<Ota>();
    extended.ReconnectVoiceGateway();
    RunWorker(extended);
    assert(extended.provisions_reconnect_attempts_ == 0);
    assert(extended.provisions_reconnect_wait_ticks_ == 0);
    assert(extended.ota_ == nullptr);

    Application disconnected;
    disconnected.network_connected_ = false;
    disconnected.protocol->open_result = true;
    disconnected.ReconnectVoiceGateway();
    RunWorker(disconnected);
    assert(disconnected.provisions_reconnect_attempts_ == 1);
    assert(disconnected.protocol->closes == 1);

    Application stale;
    auto old_protocol = stale.protocol;
    stale.ReconnectVoiceGateway();
    stale.protocol = std::make_shared<Protocol>();
    RunWorker(stale);
    assert(!stale.provisions_network_busy_);
    assert(stale.provisions_reconnect_attempts_ == 0);
    assert(stale.protocol->opens == 0 && old_protocol->opens == 1);

    Application task_failure;
    task_create_result = 0;
    task_failure.ReconnectVoiceGateway();
    assert(!task_failure.provisions_network_busy_);
    assert(task_failure.provisions_reconnect_attempts_ == 0);
    assert(task_failure.provisions_reconnect_wait_ticks_ >= 16 &&
           task_failure.provisions_reconnect_wait_ticks_ <= 19);

    Application allocation_failure;
    task_create_result = pdPASS;
    fail_nothrow_allocation = true;
    allocation_failure.ReconnectVoiceGateway();
    fail_nothrow_allocation = false;
    assert(!allocation_failure.provisions_network_busy_);
    assert(allocation_failure.provisions_reconnect_attempts_ == 0);
    assert(allocation_failure.provisions_reconnect_wait_ticks_ >= 16 &&
           allocation_failure.provisions_reconnect_wait_ticks_ <= 19);
}
""".replace("__RECONNECT__", reconnect)
        )


if __name__ == "__main__":
    unittest.main()
