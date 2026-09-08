"""Behavioral checks through the actual decoder, scheduler, alarm engine and LVGL face."""

import hashlib
import tempfile
import unittest

from run_service_schedule_demo import Demo, build


class ServiceScheduleDemoTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.executable = build()

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.demo = Demo(self.executable, self.directory.name)
        self.addCleanup(self.demo.close)

    def advance(self, count=1):
        for _ in range(count):
            state = self.demo.command("next")
            self.assertTrue(state["accepted"], state)
        return state

    def test_actual_lvgl_face_renders_three_timers_and_service(self):
        state = self.demo.state
        self.assertTrue(state["frame_written"])
        self.assertGreater(state["flushes"], 0)
        self.assertTrue(self.demo.png.startswith(b"\x89PNG\r\n\x1a\n"))
        self.assertGreater(len(self.demo.png), 4000)
        for label in ("Rice", "Sauce", "Bread", "19:00", "DINNER / DEMO"):
            self.assertIn(label, state["labels"])
        self.assertFalse(state["alarm_active"])

    def test_service_edit_only_moves_linked_setup_and_updates_pixels(self):
        before = self.demo.state
        before_png = hashlib.sha256(self.demo.png).digest()
        after = self.advance()
        self.assertEqual(after["service_at_ms"] - before["service_at_ms"], 30 * 60_000)
        self.assertEqual(after["deadlines"]["Setup"] - before["deadlines"]["Setup"], 30 * 60_000)
        for label in ("Rice", "Sauce", "Bread", "Fixed reminder"):
            self.assertEqual(after["deadlines"][label], before["deadlines"][label])
        self.assertIn("19:30", after["labels"])
        self.assertNotEqual(hashlib.sha256(self.demo.png).digest(), before_png)

    def test_elapsed_clock_updates_real_countdown_without_a_checkpoint(self):
        before = self.demo.state
        before_png = hashlib.sha256(self.demo.png).digest()
        after = self.demo.command("tick 1000")
        self.assertEqual(after["now_ms"] - before["now_ms"], 1000)
        self.assertEqual(after["deadlines"], before["deadlines"])
        self.assertIn("1:59", after["labels"])
        self.assertNotEqual(hashlib.sha256(self.demo.png).digest(), before_png)

    def test_due_offline_alarm_ack_only_silences_one_and_stays_pending(self):
        state = self.advance(4)
        self.assertFalse(state["connected"])
        self.assertEqual(set(state["due"]), {"Fixed reminder", "Rice"})
        self.assertTrue(state["alarm_active"])
        state = self.demo.command("ack")
        self.assertTrue(state["accepted"])
        self.assertEqual(len(state["due"]), 1)
        self.assertTrue(state["alarm_active"], "one ACK must not silence another due item")
        self.assertEqual(len(state["pending"]), 1)
        state = self.demo.command("ack")
        self.assertFalse(state["alarm_active"])
        self.assertEqual(state["alarm_change"], "stop")
        self.assertEqual(len(state["pending"]), 2)
        self.assertTrue(any("OFFLINE" in label and "ACK PENDING 2" in label for label in state["labels"]))
        self.assertFalse(self.demo.command("receipt")["accepted"], "offline cannot accept a receipt")

    def test_reconnect_requires_exact_receipt_and_old_replay_preserves_ack(self):
        self.advance(3)
        self.demo.command("ack")
        self.advance(4)
        state = self.demo.state
        self.assertTrue(state["connected"])
        self.assertEqual(len(state["pending"]), 1)
        self.assertFalse(state["receipt_confirmed"])
        pending = state["pending"]
        self.assertFalse(self.demo.command("wrong_receipt")["accepted"])
        self.assertEqual(self.demo.state["pending"], pending)
        state = self.advance()  # one exact synthetic authoritative receipt
        self.assertEqual(state["pending"], [])
        self.assertTrue(state["receipt_confirmed"])
        self.assertTrue(any("MOCK ACK RECEIPT" in label for label in state["labels"]))
        revision, deadlines, due = state["snapshot_revision"], state["deadlines"], state["due"]
        state = self.advance()  # stale initial snapshot
        self.assertIn("Old snapshot rejected", state["stage"])
        self.assertEqual(state["snapshot_revision"], revision)
        self.assertEqual(state["deadlines"], deadlines)
        self.assertEqual(state["due"], due)
        self.assertNotIn("Fixed reminder", state["due"])

    def test_acknowledged_alarm_restarts_for_next_due_timer(self):
        state = self.advance(3)
        self.assertEqual(state["alarm_change"], "start")
        state = self.demo.command("ack")
        self.assertEqual(state["alarm_change"], "stop")
        state = self.advance()
        self.assertEqual(state["due"], ["Rice"])
        self.assertEqual(state["alarm_change"], "start")

    def test_linked_setup_due_at_new_time_and_clock_rollback_is_visible(self):
        self.advance(10)
        self.assertIn("Setup", self.demo.state["due"])
        state = self.advance()
        self.assertFalse(state["clock_trusted"])
        self.assertTrue(any("CHECK TIME" in label for label in state["labels"]))

    def test_reset_restores_original_fixture_without_old_acks(self):
        self.advance(4)
        self.demo.command("ack")
        state = self.demo.command("reset")
        self.assertIn("19:00", state["labels"])
        self.assertEqual(state["pending"], [])
        self.assertFalse(state["alarm_active"])
        self.assertTrue(state["connected"])


if __name__ == "__main__":
    unittest.main()
