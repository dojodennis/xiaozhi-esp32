"""Compile the firmware reply fence and exercise interruption without a new capture."""

import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which("c++"), "host C++ compiler is unavailable")
class ReplyTurnReviewTests(unittest.TestCase):
    def test_abandoning_reply_disables_old_id_before_the_next_capture_begins(self):
        source = textwrap.dedent(
            r"""
            #include "provisions_reply_turn.h"
            #include <cassert>
            #include <thread>
            #include <vector>

            int main() {
                ProvisionsReplyTurn turn;
                assert(turn.id() == 0);
                assert(!turn.IsCurrent(0));
                assert(!turn.IsCurrent(1));
                assert(turn.Begin());
                const auto first = turn.id();
                assert(first == 1 && turn.IsCurrent(first));

                // Talk interrupts Working, then releases before deferred audio
                // capture begins. No new ID exists yet; old replies must still
                // be invalid while the microphone-held gate is false again.
                turn.Invalidate();
                assert(turn.id() == first);
                assert(!turn.IsCurrent(first));
                turn.Invalidate();
                assert(!turn.IsCurrent(first));

                assert(turn.Begin());
                assert(turn.id() == first + 1);
                assert(turn.IsCurrent(first + 1));
                assert(!turn.IsCurrent(first));
                assert(!turn.IsCurrent(first + 2));

                // Concurrent starts cannot lose increments or reuse a reply ID.
                ProvisionsReplyTurn concurrent;
                std::vector<std::thread> threads;
                for (int t = 0; t < 4; ++t) {
                    threads.emplace_back([&concurrent]() {
                        for (int i = 0; i < 1000; ++i) assert(concurrent.Begin());
                    });
                }
                for (auto& thread : threads) thread.join();
                assert(concurrent.id() == 4000);
                assert(concurrent.IsCurrent(4000));
                assert(!concurrent.IsCurrent(3999));
                concurrent.Invalidate();
                assert(!concurrent.IsCurrent(4000));
                assert(concurrent.Begin());
                assert(concurrent.id() == 4001 && concurrent.IsCurrent(4001));
            }
            """
        )
        with tempfile.TemporaryDirectory() as directory:
            test_source = Path(directory) / "reply_turn_review.cc"
            executable = Path(directory) / "reply_turn_review"
            test_source.write_text(source, encoding="utf-8")
            compiled = subprocess.run(
                [
                    "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                    "-I", str(ROOT / "main"), str(test_source), "-o", str(executable),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            subprocess.run([str(executable)], check=True, capture_output=True, text=True)


if __name__ == "__main__":
    unittest.main()
