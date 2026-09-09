"""Exercise actual board adapter methods at publication/render/gesture boundaries.

Display and coordinator hooks substitute only the surrounding platforms. This
verifies completed UI mutation, not physical display scanout or button timing.
"""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def method(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


PREFIX = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include "local_feedback_trace.h"
#define TAG "test"
template<typename... Args> void Log(Args&&...) {}
#define ESP_LOGI Log

int64_t esp_timer_get_time() { return 1000000; }
namespace orbit::service_schedule {
struct WorkerPublication { uint64_t sequence; };
using Publication = std::shared_ptr<const WorkerPublication>;
struct HardwareBench {
    Publication view, next, acknowledged;
    int rejected = 0;
    void RejectGesture() { ++rejected; }
    void Poll(int64_t) { if (next) view = std::exchange(next, {}); }
    const Publication& current() { return view; }
    std::string Status() { return "BENCH"; }
    void Blue(Publication shown) { acknowledged = std::move(shown); }
    void Yellow(Publication shown, int64_t) { acknowledged = std::move(shown); }
};
}
using namespace orbit::service_schedule;
struct Application {
    struct Audio {
        LocalFeedbackTrace trace;
        auto TakeBenchAudioTrace() { return trace.Pop(); }
    } audio;
    Audio& GetAudioService() { return audio; }
    std::deque<std::function<void()>> tasks;
    static Application& GetInstance() { static Application app; return app; }
    void Schedule(std::function<void()> action) { tasks.push_back(std::move(action)); }
    void Drain() {
        auto current = std::move(tasks);
        tasks.clear();
        for (auto& action : current) action();
    }
};
struct Display {
    std::function<void()> during_render;
    void RenderHardwareBench(const Publication&, const std::string&) {
        if (during_render) {
            auto interrupt = std::exchange(during_render, {});
            interrupt();
        }
    }
};
struct Board {
    std::unique_ptr<HardwareBench> bench_ = std::make_unique<HardwareBench>();
    std::atomic<HardwareBench*> bench_published_{bench_.get()};
    Publication bench_rendered_;
    std::atomic<bool> bench_gesture_queued_{false}, bench_gesture_rejected_{false};
    bool bench_motor_fault_ = false;
    uint64_t bench_display_sequence_ = 0;
    std::string bench_display_status_;
    Display display;
    Display* display_ = &display;
'''

SUFFIX = r'''
};
int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string test = argv[1];
    Board board;
    auto& app = Application::GetInstance();
    const auto first = std::make_shared<const WorkerPublication>(WorkerPublication{1});
    const auto second = std::make_shared<const WorkerPublication>(WorkerPublication{2});
    board.bench_->view = first;
    board.PollHardwareBench();
    assert(board.bench_rendered_ == first);
    if (test == "render_boundary") {
        board.bench_->next = second;
        // Model a physical callback arriving after the coordinator has a newer
        // publication but before the board's display mutation has completed.
        board.display.during_render = [&] { board.QueueBenchGesture(true); };
        board.PollHardwareBench();
        assert(board.bench_rendered_ == second && app.tasks.size() == 1);
        app.Drain();
        assert(board.bench_->acknowledged == first);
        board.QueueBenchGesture(true);
        app.Drain();
        assert(board.bench_->acknowledged == second);
    } else if (test == "bounded_gesture_backlog") {
        board.QueueBenchGesture(true);
        for (int i = 0; i < 1000; ++i) board.QueueBenchGesture(true);
        assert(app.tasks.size() == 1 && !board.bench_->acknowledged);
        app.Drain();
        assert(board.bench_->acknowledged == first && board.bench_->rejected == 1);
        assert(!board.bench_gesture_queued_ && !board.bench_gesture_rejected_);
        board.bench_->acknowledged = {};
        board.QueueBenchGesture(false);
        assert(app.tasks.size() == 1);
        app.Drain();
        assert(board.bench_->acknowledged == first);
    } else if (test == "owner_fence") {
        board.QueueBenchGesture(true);
        board.bench_published_ = nullptr;
        app.Drain();
        assert(!board.bench_->acknowledged && !board.bench_gesture_queued_);
        board.QueueBenchGesture(true);
        assert(app.tasks.empty());
    } else {
        assert(false);
    }
}
'''


class HardwareBenchAdapterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        board = (ROOT / "main/boards/m5stack/stopwatch/m5stack_stopwatch.cc").read_text()
        actual = "\n".join(method(board, signature) for signature in (
            "void PollHardwareBench()", "void QueueBenchGesture(bool blue)",
        ))
        cls.directory = tempfile.TemporaryDirectory(prefix="orbit-bench-adapter-")
        cls.addClassCleanup(cls.directory.cleanup)
        path = Path(cls.directory.name)
        source = path / "review.cc"
        source.write_text(PREFIX + actual + SUFFIX)
        cls.executable = path / "review"
        compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            raise RuntimeError("A host C++17 compiler is required")
        result = subprocess.run([
            compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
            "-I", str(ROOT / "main/audio"), str(source), "-o", str(cls.executable),
        ], capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    def run_case(self, case):
        environment = os.environ.copy()
        leaks = 0 if sys.platform == "darwin" else 1
        environment["ASAN_OPTIONS"] = f"detect_leaks={leaks}:halt_on_error=1"
        environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        result = subprocess.run(
            [str(self.executable), case], capture_output=True, text=True,
            env=environment, timeout=10,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_gesture_binds_only_completed_render(self):
        self.run_case("render_boundary")

    def test_repeated_gestures_keep_a_bounded_callback_backlog(self):
        self.run_case("bounded_gesture_backlog")

    def test_replaced_owner_drops_queued_gesture(self):
        self.run_case("owner_fence")


if __name__ == "__main__":
    unittest.main()
