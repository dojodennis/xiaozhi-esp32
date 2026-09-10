"""Host checks for the unbounded, jittered Provisions gateway reconnect schedule."""

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


# Mirror of provisions_reconnect_policy.h, kept in Python so the table below
# is derived independently of the header under test.
MAXIMUM_TICKS, FLOOR_TICKS, FLOOR_AFTER, JITTER_DIVISOR = 30, 10, 3, 4


def base_ticks(attempts: int) -> int:
    base = 1
    for _ in range(attempts):
        if base >= MAXIMUM_TICKS:
            break
        base *= 2
    return min(base, MAXIMUM_TICKS)


def delay_ticks(attempts: int, rejections: int, entropy: int) -> int:
    base = base_ticks(attempts)
    quarter = base // JITTER_DIVISOR
    wait = base - quarter + entropy % (2 * quarter + 1)
    return max(wait, FLOOR_TICKS if rejections >= FLOOR_AFTER else 1)


def seeded_entropy(seed: int, count: int) -> list:
    values, state = [], seed
    for _ in range(count):
        state = (state * 1103515245 + 12345) & 0x7FFFFFFF
        values.append(state)
    return values


class ProvisionsReconnectPolicyTests(unittest.TestCase):
    def test_schedule_doubles_from_one_second_caps_at_thirty_and_jitters_a_quarter(self) -> None:
        expected = {0: (1, 1), 1: (2, 2), 2: (3, 5), 3: (6, 10), 4: (12, 20), 5: (23, 37),
                    6: (23, 37), 40: (23, 37)}
        cases = "\n".join(
            f"    assert(MinimumDelayTicks({attempt}, 0) == {low} && MaximumDelayTicks({attempt}, 0) == {high});"
            for attempt, (low, high) in expected.items()
        )
        run_cpp(
            r"""
#include <cassert>
#include <limits>
#include "provisions_reconnect_policy.h"

int main() {
    using namespace provisions::reconnect;
    static_assert(kMaximumBackoffTicks == 30, "cap is thirty seconds");
    static_assert(kRejectionFloorTicks == 10 && kRejectionFloorAfter == 3, "rate-limit floor");
__CASES__
    // Every entropy value lands inside the window, both edges are reachable,
    // and no wait is ever zero.
    for (int attempt = 0; attempt <= 12; ++attempt) {
        bool low_seen = false, high_seen = false;
        for (uint32_t entropy = 0; entropy < 64; ++entropy) {
            const int wait = DelayTicks(attempt, 0, entropy);
            assert(wait >= 1);
            assert(wait >= MinimumDelayTicks(attempt, 0));
            assert(wait <= MaximumDelayTicks(attempt, 0));
            low_seen |= wait == MinimumDelayTicks(attempt, 0);
            high_seen |= wait == MaximumDelayTicks(attempt, 0);
        }
        assert(low_seen && high_seen);
    }
    assert(DelayTicks(std::numeric_limits<int>::max(), 0, 0) == 23);
    assert(DelayTicks(std::numeric_limits<int>::max(), 0, 14) == 37);
    assert(DelayTicks(-5, 0, 9) == 1);
    assert(WorkerFailureDelayTicks(0) == 23 && WorkerFailureDelayTicks(14) == 37);

    // Attempts saturate instead of wrapping; the schedule never terminates.
    auto sixth = AfterGatewayFailure(5, 0, 0);
    assert(sixth.attempts == 6 && sixth.wait_ticks == 23);
    auto saturated = AfterGatewayFailure(std::numeric_limits<int>::max(), 0, 3);
    assert(saturated.attempts == std::numeric_limits<int>::max());
    assert(saturated.wait_ticks == 26);
}
""".replace("__CASES__", cases)
        )

    def test_seeded_jitter_is_deterministic_and_matches_the_reference_model(self) -> None:
        entropy = seeded_entropy(2026, 10)
        expected = [delay_ticks(attempt, 0, value) for attempt, value in enumerate(entropy)]
        self.assertEqual(expected, [1, 2, 3, 9, 12, 29, 27, 29, 35, 29])
        run_cpp(
            r"""
#include <cassert>
#include <cstdint>
#include "provisions_reconnect_policy.h"

int main() {
    using namespace provisions::reconnect;
    const int expected[] = {__EXPECTED__};
    uint32_t state = 2026;
    int attempts = 0;
    for (int step = 0; step < 10; ++step) {
        state = (state * 1103515245u + 12345u) & 0x7fffffffu;
        const auto retry = AfterGatewayFailure(attempts, 0, state);
        assert(retry.wait_ticks == expected[step]);
        attempts = retry.attempts;
    }
    assert(attempts == 10);
}
""".replace("__EXPECTED__", ", ".join(map(str, expected)))
        )

    def test_three_consecutive_rejections_raise_the_floor_to_ten_seconds(self) -> None:
        run_cpp(
            r"""
#include <cassert>
#include "provisions_reconnect_policy.h"

int main() {
    using namespace provisions::reconnect;
    // Fewer than three rejections: the ordinary schedule.
    for (int rejections = 0; rejections < 3; ++rejections) {
        assert(DelayTicks(0, rejections, 0) == 1);
        assert(DelayTicks(2, rejections, 0) == 3);
    }
    // Three or more: never faster than ten seconds, jitter kept above it.
    for (int rejections = 3; rejections < 6; ++rejections) {
        for (int attempt = 0; attempt < 4; ++attempt) {
            for (uint32_t entropy = 0; entropy < 32; ++entropy)
                assert(DelayTicks(attempt, rejections, entropy) >= 10);
        }
        assert(MinimumDelayTicks(0, rejections) == 10 && MaximumDelayTicks(0, rejections) == 10);
        assert(MinimumDelayTicks(3, rejections) == 10 && MaximumDelayTicks(3, rejections) == 10);
        assert(MinimumDelayTicks(4, rejections) == 12 && MaximumDelayTicks(4, rejections) == 20);
    }
    // The floor never lowers a wait that is already above it.
    assert(DelayTicks(5, 3, 14) == 37);
    assert(MinimumDelayTicks(5, 3) == 23 && MaximumDelayTicks(5, 3) == 37);
}
"""
        )

    def test_actual_maintenance_never_gives_up_and_tracks_rejections(self) -> None:
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
    std::deque<bool> rejections;
    bool last_rejected = false;
    bool IsAudioChannelOpened() const { return open; }
    bool OpenAudioChannel() {
        ++open_calls;
        const bool result = results.empty() ? false : results.front();
        if (!results.empty()) results.pop_front();
        last_rejected = !rejections.empty() && rejections.front();
        if (!rejections.empty()) rejections.pop_front();
        open = result;
        return result;
    }
    void CloseAudioChannel() { open = false; }
};
struct WebsocketProtocol : Protocol {
    bool IsGatewayHeartbeatExpired() const { return false; }
    bool SendGatewayHeartbeat() { return true; }
    bool LastOpenRejected() const { return last_rejected; }
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
    int provisions_gateway_rejections_ = 0;
    void* activation_task_handle_ = nullptr;
    bool aborted_ = false;
    bool has_server_time_ = false;
    int state = kDeviceStateIdle;
    AudioService audio_service_;
    int offline_notes = 0;
    int online_notes = 0;

    std::shared_ptr<Protocol> GetProtocol() { return protocol; }
    int GetDeviceState() const { return state; }
    void SetDeviceState(int value) { state = value; }
    void SetProvisionsResponsePending(bool value) { provisions_response_pending_ = value; }
    void InvalidateProvisionsTtsTurn() {}
    void NoteProvisionsOffline() { ++offline_notes; }
    void NoteProvisionsOnline() { ++online_notes; }
    const char* GetProvisionsIdleStatus() const { return "Ready"; }
    void Alert(const char*, const char*, const char*, std::string_view) {}
    void HandleProvisionsGatewayMaintenance();
};

__HANDLER__

int run_until(Application& app, int expected_open_calls) {
    int ticks = 0;
    for (; ticks < 5000 && app.protocol->open_calls < expected_open_calls; ++ticks)
        app.HandleProvisionsGatewayMaintenance();
    return ticks;
}

int main() {
    using namespace provisions::reconnect;
    // A confirmed image keeps trying well past the old five-attempt wall and
    // comes back to Ready without a reboot.
    Application confirmed;
    for (int i = 0; i < 11; ++i) confirmed.protocol->results.push_back(false);
    confirmed.protocol->results.push_back(true);
    const int ticks = run_until(confirmed, 12);
    assert(confirmed.protocol->open);
    assert(confirmed.protocol->open_calls == 12);
    assert(confirmed.provisions_reconnect_attempts_ == 0);
    assert(confirmed.provisions_reconnect_wait_ticks_ == 0);
    assert(confirmed.provisions_gateway_rejections_ == 0);
    assert(Board::GetInstance().display.status == std::string("Ready"));
    assert(restarts == 0);
    // Eleven failures wait at least the sum of the minimum schedule and never
    // more than the maximum, so the cap is real.
    int minimum = 0, maximum = 0;
    for (int attempt = 0; attempt < 11; ++attempt) {
        minimum += MinimumDelayTicks(attempt, 0);
        maximum += MaximumDelayTicks(attempt, 0);
    }
    assert(ticks >= minimum && ticks <= maximum + 12);

    // The wait between attempts is the policy's wait, not the old 1 << attempt.
    Application spacing;
    spacing.protocol->results = {false, false, false, false, false, false, false};
    for (int attempt = 0; attempt < 7; ++attempt) {
        run_until(spacing, attempt + 1);
        assert(spacing.provisions_reconnect_wait_ticks_ >= MinimumDelayTicks(attempt, 0));
        assert(spacing.provisions_reconnect_wait_ticks_ <= MaximumDelayTicks(attempt, 0));
    }
    assert(spacing.provisions_reconnect_attempts_ == 7);

    // A pending OTA image still rolls back after the fifth failure.
    Application pending;
    pending.ota_ = std::make_unique<Ota>();
    pending.ota_->pending = true;
    pending.protocol->results = {false, false, false, false, false, true};
    run_until(pending, 5);
    while (restarts == 0) pending.HandleProvisionsGatewayMaintenance();
    assert(pending.protocol->open_calls == 5);
    assert(pending.provisions_reconnect_attempts_ == 5);
    assert(restarts == 1);

    // Three consecutive gateway rejections raise the floor to ten seconds; a
    // link failure in between resets the run; success clears everything.
    Application rejected;
    rejected.protocol->results = {false, false, false, false, true};
    rejected.protocol->rejections = {true, true, true, false, false};
    run_until(rejected, 2);
    assert(rejected.provisions_gateway_rejections_ == 2);
    assert(rejected.provisions_reconnect_wait_ticks_ == 2);  // attempt 1, no floor yet
    run_until(rejected, 3);
    assert(rejected.provisions_gateway_rejections_ == 3);
    assert(rejected.provisions_reconnect_wait_ticks_ >= 10);  // attempt 2 would be 3..5
    run_until(rejected, 4);
    assert(rejected.provisions_gateway_rejections_ == 0);  // a link failure ends the run
    assert(rejected.provisions_reconnect_wait_ticks_ <= 10);  // attempt 3: 6..10
    run_until(rejected, 5);
    assert(rejected.protocol->open);
    assert(rejected.provisions_gateway_rejections_ == 0);
    assert(rejected.provisions_reconnect_attempts_ == 0);

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
        for path in (maintenance, reconnect):
            self.assertIn("AfterGatewayFailure(", path)
            self.assertIn("LastOpenRejected()", path)
            self.assertIn("provisions_gateway_rejections_ = 0", path)
        self.assertNotIn("provisions_recorder_.reset", maintenance + reconnect)
        self.assertNotIn("1 << attempt", maintenance + reconnect)
        self.assertIn("kProvisionsMaximumReconnectAttempts = 5", application)
        self.assertIn("ota_->IsCurrentVersionPendingVerification()", maintenance)

    def test_rejection_is_classified_from_upgrade_status_and_refused_hello(self) -> None:
        protocol = (ROOT / "main/protocols/websocket_protocol.cc").read_text(encoding="utf-8")
        transport = (ROOT / "main/protocols/provisions_websocket.cc").read_text(encoding="utf-8")
        signature = "bool WebsocketProtocol::OpenAudioChannelImpl()"
        if signature not in protocol:
            signature = "bool WebsocketProtocol::OpenAudioChannel()"
        open_channel = method(protocol, signature)
        reject = method(protocol, "void WebsocketProtocol::RejectServerHello(")
        self.assertIn("last_open_rejected_.store(false)", open_channel)
        self.assertIn("upgrade_status == 401 || upgrade_status == 403 || upgrade_status == 429",
                      open_channel)
        self.assertIn("last_open_rejected_.store(true)", reject)
        self.assertIn("upgrade_status.store(esp_transport_ws_get_upgrade_request_status(websocket))",
                      transport)
        self.assertIn("int ProvisionsWebSocket::GetLastUpgradeStatus() const", transport)

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
    virtual ~Protocol() = default;
    bool open_result = false;
    bool rejected = false;
    int opens = 0;
    int closes = 0;
    bool OpenAudioChannel() { ++opens; return open_result; }
    void CloseAudioChannel() { ++closes; }
};
struct WebsocketProtocol : Protocol {
    bool LastOpenRejected() const { return rejected; }
};
struct Ota {
    bool marked = false;
    bool HasServerTime() const { return true; }
    void MarkCurrentVersionValid() { marked = true; }
};
struct Application {
    std::shared_ptr<WebsocketProtocol> protocol = std::make_shared<WebsocketProtocol>();
    std::unique_ptr<Ota> ota_;
    std::atomic<bool> provisions_network_busy_{false};
    std::atomic<bool> network_connected_{true};
    int provisions_reconnect_attempts_ = 0;
    int provisions_reconnect_wait_ticks_ = 0;
    int provisions_gateway_rejections_ = 0;
    bool has_server_time_ = false;
    int state = kDeviceStateIdle;
    int offline_notes = 0;
    int online_notes = 0;
    std::deque<std::function<void()>> scheduled;

    std::shared_ptr<Protocol> GetProtocol() { return protocol; }
    int GetDeviceState() const { return state; }
    const char* GetProvisionsIdleStatus() const { return "Ready"; }
    void NoteProvisionsOffline() { ++offline_notes; }
    void NoteProvisionsOnline() { ++online_notes; }
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
    using namespace provisions::reconnect;
    Application single;
    single.ReconnectVoiceGateway();
    assert(single.provisions_network_busy_ && task_create_calls == 1);
    single.ReconnectVoiceGateway();
    assert(task_create_calls == 1);
    RunWorker(single);
    assert(!single.provisions_network_busy_);
    assert(single.provisions_reconnect_attempts_ == 1);
    assert(single.provisions_reconnect_wait_ticks_ == 1);
    assert(single.protocol->closes == 1);

    Application extended;
    for (int failure = 0; failure < 9; ++failure) {
        extended.ReconnectVoiceGateway();
        RunWorker(extended);
        assert(extended.provisions_reconnect_wait_ticks_ >= MinimumDelayTicks(failure, 0));
        assert(extended.provisions_reconnect_wait_ticks_ <= MaximumDelayTicks(failure, 0));
    }
    assert(extended.provisions_reconnect_attempts_ == 9);
    assert(extended.provisions_reconnect_wait_ticks_ >= 23 &&
           extended.provisions_reconnect_wait_ticks_ <= 37);
    extended.protocol->open_result = true;
    extended.ota_ = std::make_unique<Ota>();
    extended.ReconnectVoiceGateway();
    RunWorker(extended);
    assert(extended.provisions_reconnect_attempts_ == 0);
    assert(extended.provisions_reconnect_wait_ticks_ == 0);
    assert(extended.provisions_gateway_rejections_ == 0);
    assert(extended.ota_ == nullptr);

    Application refused;
    refused.protocol->rejected = true;
    for (int failure = 0; failure < 3; ++failure) {
        refused.ReconnectVoiceGateway();
        RunWorker(refused);
    }
    assert(refused.provisions_gateway_rejections_ == 3);
    assert(refused.provisions_reconnect_wait_ticks_ >= 10);
    refused.protocol->rejected = false;
    refused.ReconnectVoiceGateway();
    RunWorker(refused);
    assert(refused.provisions_gateway_rejections_ == 0);

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
    stale.protocol = std::make_shared<WebsocketProtocol>();
    RunWorker(stale);
    assert(!stale.provisions_network_busy_);
    assert(stale.provisions_reconnect_attempts_ == 0);
    assert(stale.protocol->opens == 0 && old_protocol->opens == 1);

    Application task_failure;
    task_create_result = 0;
    task_failure.ReconnectVoiceGateway();
    assert(!task_failure.provisions_network_busy_);
    assert(task_failure.provisions_reconnect_attempts_ == 0);
    assert(task_failure.provisions_reconnect_wait_ticks_ >= 23 &&
           task_failure.provisions_reconnect_wait_ticks_ <= 37);

    Application allocation_failure;
    task_create_result = pdPASS;
    fail_nothrow_allocation = true;
    allocation_failure.ReconnectVoiceGateway();
    fail_nothrow_allocation = false;
    assert(!allocation_failure.provisions_network_busy_);
    assert(allocation_failure.provisions_reconnect_attempts_ == 0);
    assert(allocation_failure.provisions_reconnect_wait_ticks_ >= 23 &&
           allocation_failure.provisions_reconnect_wait_ticks_ <= 37);
}
""".replace("__RECONNECT__", reconnect)
        )


if __name__ == "__main__":
    unittest.main()
