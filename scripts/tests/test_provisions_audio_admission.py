"""Compile the production admission primitive and exercise ownership/closure races."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

PROGRAM = r'''
#include "audio/provisions_audio_admission.h"
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
using namespace provisions::audio_admission;
static_assert(!std::is_copy_constructible_v<Reservation>);
static_assert(!std::is_copy_assignable_v<Reservation>);
static_assert(!std::is_move_constructible_v<Reservation>);
static_assert(!std::is_move_assignable_v<Reservation>);

Uuid uuid(uint8_t value) { Uuid result{}; result[15]=value; return result; }
FenceIdentity owner(uint64_t epoch=7, uint64_t sequence=3, uint8_t lease=2) {
    FenceIdentity value;
    value.device_id=uuid(1); value.lease_id=uuid(lease); value.playback_id=uuid(epoch);
    value.request_id=uuid(4); value.route_epoch=uuid(5); value.device_connection_id=uuid(6);
    value.checkpoint_sha256[31]=static_cast<uint8_t>(epoch);
    value.fence_epoch=epoch; value.sequence=sequence; value.response_revision=1;
    return value;
}
TimerIdentity timer(bool alarm=false, uint8_t lease=20) {
    TimerIdentity value; value.lease_id=uuid(lease);
    if(alarm) { value.kind=TimerKind::Alarm; value.playback_id=uuid(21);
        value.timer_id=uuid(22); value.timer_revision=2; value.attempt=3; }
    return value;
}
void acknowledge(Gate& gate, Generation generation) {
    for(unsigned i=0;i<static_cast<unsigned>(Acknowledgement::Count);++i)
        assert(gate.Acknowledge(static_cast<Acknowledgement>(i),generation));
}
Generation open(Gate& gate, const FenceIdentity& identity=owner()) {
    const auto generation=gate.BeginClose(); assert(gate.Hold(identity,generation));
    acknowledge(gate,generation); assert(gate.Snapshot(identity).metadata_closed);
    assert(gate.OpenAfterTerminal(identity,generation)); return generation;
}
struct Barrier {
    std::mutex mutex; std::condition_variable cv; unsigned count=0; bool released=false;
    void arrive() { std::unique_lock<std::mutex> lock(mutex); ++count; cv.notify_all();
        cv.wait(lock,[&]{return released;}); }
    void wait(unsigned target) { std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock,[&]{return count==target;}); }
    void release() { std::lock_guard<std::mutex> lock(mutex); released=true; cv.notify_all(); }
};
int main(int argc,char** argv) {
    assert(argc==2); const std::string name=argv[1];
    if(name=="initial") {
        Gate gate; Reservation token; auto state=gate.Snapshot(owner());
        assert(state.blocked&&!state.faulted&&!state.metadata_closed&&!state.owner_matches);
        assert(state.active==0&&state.acknowledgements==0);
        assert(!gate.Reserve(Producer::Capture,token));
        assert(!gate.AllowsPublication(token)); assert(!gate.Complete(token));
        acknowledge(gate,state.generation); assert(!gate.OpenAfterTerminal(owner(),state.generation));
        auto invalid=owner(); invalid.device_id={}; assert(!gate.Hold(invalid,state.generation));
        assert(!gate.Acknowledge(static_cast<Acknowledgement>(255),state.generation));
        assert(gate.Hold(owner(),state.generation)); assert(gate.Snapshot(owner()).metadata_closed);
        assert(!gate.Snapshot().metadata_closed); assert(gate.Snapshot().blocked);
    } else if(name=="every_ack") {
        for(unsigned missing=0;missing<5;++missing) {
            Gate gate; const auto generation=gate.BeginClose(); assert(gate.Hold(owner(),generation));
            for(unsigned i=0;i<5;++i) if(i!=missing)
                assert(gate.Acknowledge(static_cast<Acknowledgement>(i),generation));
            for(int i=0;i<100;++i) assert(!gate.Snapshot(owner()).metadata_closed);
            assert(!gate.OpenAfterTerminal(owner(),generation));
            assert(gate.Acknowledge(static_cast<Acknowledgement>(missing),generation));
            assert(gate.OpenAfterTerminal(owner(),generation));
        }
    } else if(name=="idempotent_close") {
        Gate gate; const auto first=open(gate); const auto closing=gate.BeginClose();
        assert(closing==first+1); assert(gate.Hold(owner(),closing));
        assert(gate.Acknowledge(Acknowledgement::Boot,closing));
        for(int i=0;i<1000;++i) assert(gate.BeginClose()==closing);
        assert(gate.Snapshot().acknowledgements==1); acknowledge(gate,closing);
        const auto newer=gate.Invalidate(); assert(newer==closing+1);
        for(unsigned i=0;i<5;++i)
            assert(!gate.Acknowledge(static_cast<Acknowledgement>(i),closing));
        assert(!gate.OpenAfterTerminal(owner(),closing));
        assert(gate.Snapshot().acknowledgements==0);
        acknowledge(gate,newer); assert(gate.OpenAfterTerminal(owner(),newer));
    } else if(name=="all_producers") {
        Gate gate; open(gate); std::array<Reservation,6> tokens;
        for(size_t i=0;i<tokens.size();++i) {
            assert(gate.Reserve(static_cast<Producer>(i+1),tokens[i]));
            assert(gate.AllowsPublication(tokens[i]));
        }
        const auto generation=gate.BeginClose(); assert(gate.Hold(owner(8,4),generation));
        acknowledge(gate,generation); const auto state=gate.Snapshot(owner(8,4));
        assert(state.active==6&&!state.metadata_closed);
        for(size_t i=0;i<tokens.size();++i) {
            assert(state.active_by_producer[i+1]==1); assert(!gate.AllowsPublication(tokens[i]));
            assert(!gate.OpenAfterTerminal(owner(8,4),generation));
            assert(gate.Complete(tokens[i]));
        }
        assert(gate.Snapshot(owner(8,4)).metadata_closed);
        assert(gate.Snapshot().blocked); // Completion itself never opens.
        assert(gate.OpenAfterTerminal(owner(8,4),generation));
    } else if(name=="identity") {
        Gate gate; const auto identity=owner(); const auto generation=gate.BeginClose();
        assert(gate.Hold(identity,generation)); acknowledge(gate,generation);
        std::vector<FenceIdentity> changed;
        for(auto field : {&FenceIdentity::device_id,&FenceIdentity::lease_id,
             &FenceIdentity::playback_id,&FenceIdentity::request_id,&FenceIdentity::route_epoch,
             &FenceIdentity::device_connection_id}) {
            auto different=identity; (different.*field)[0]=1; changed.push_back(different);
        }
        auto different=identity; different.checkpoint_sha256[0]=1; changed.push_back(different);
        different=identity; ++different.fence_epoch; changed.push_back(different);
        different=identity; ++different.sequence; changed.push_back(different);
        different=identity; ++different.response_revision; changed.push_back(different);
        for(const auto& value:changed) {
            assert(!gate.Hold(value,generation)); assert(!gate.Snapshot(value).owner_matches);
            assert(!gate.OpenAfterTerminal(value,generation));
        }
        assert(gate.OpenAfterTerminal(identity,generation));
        const auto next=gate.BeginClose();
        assert(!gate.Hold(owner(8,3),next)); // Same lease sequence cannot repeat.
        assert(gate.Hold(owner(8,1,9),next)); // New lease sequence may start at one.
        assert(!gate.Hold(owner(9,2,9),next)); // Unresolved owner cannot be replaced.
        acknowledge(gate,next); assert(!gate.OpenAfterTerminal(identity,next));
        assert(gate.OpenAfterTerminal(owner(8,1,9),next));
    } else if(name=="capacity_spent_lost") {
        Gate gate; open(gate); std::array<Reservation,Gate::kCapacity+1> tokens;
        for(size_t i=0;i<Gate::kCapacity;++i) assert(gate.Reserve(Producer::Encode,tokens[i]));
        assert(!gate.Reserve(Producer::Encode,tokens.back())); assert(!gate.Snapshot().faulted);
        assert(gate.Complete(tokens[0])); assert(!gate.Reserve(Producer::Capture,tokens[0]));
        assert(gate.Reserve(Producer::Encode,tokens.back())); assert(!gate.Complete(tokens[0]));
        assert(gate.Snapshot().active==Gate::kCapacity);
        const auto generation=gate.BeginClose(); acknowledge(gate,generation);
        for(size_t i=1;i<tokens.size();++i) assert(gate.Complete(tokens[i]));
        assert(gate.OpenAfterTerminal(owner(),generation));
        { Reservation lost; assert(gate.Reserve(Producer::Output,lost)); }
        const auto closed=gate.BeginClose(); acknowledge(gate,closed);
        assert(gate.Snapshot(owner()).active==1); assert(!gate.OpenAfterTerminal(owner(),closed));
    } else if(name=="old_completion_new_press") {
        Gate gate; const auto first=open(gate); Reservation old_press,new_press;
        assert(gate.Reserve(Producer::Capture,old_press)); assert(gate.Complete(old_press));
        assert(gate.Reserve(Producer::Capture,new_press));
        assert(!gate.Complete(old_press)); assert(gate.AllowsPublication(new_press));
        const auto generation=gate.BeginClose(); assert(generation!=first);
        acknowledge(gate,generation); assert(!gate.Complete(old_press));
        assert(!gate.OpenAfterTerminal(owner(),generation)); assert(gate.Complete(new_press));
        assert(gate.Snapshot().blocked); assert(gate.OpenAfterTerminal(owner(),generation));
    } else if(name=="simultaneous") {
        Gate gate; open(gate); std::array<Reservation,Gate::kCapacity> tokens;
        Barrier held; std::vector<std::thread> workers;
        for(size_t i=0;i<tokens.size();++i) workers.emplace_back([&,i]{
            assert(gate.Reserve(static_cast<Producer>(1+i%6),tokens[i])); held.arrive();
            assert(!gate.AllowsPublication(tokens[i])); assert(gate.Complete(tokens[i])); });
        held.wait(Gate::kCapacity); assert(gate.Snapshot().active==Gate::kCapacity);
        const auto generation=gate.BeginClose(); acknowledge(gate,generation);
        assert(!gate.OpenAfterTerminal(owner(),generation)); held.release();
        for(auto& worker:workers) worker.join();
        assert(gate.Snapshot(owner()).metadata_closed); assert(gate.Snapshot().blocked);
    } else if(name=="cross_gate") {
        Gate a,b; open(a); open(b); Reservation token; Barrier start;
        bool admitted_a=false,admitted_b=false;
        std::thread one([&]{start.arrive(); admitted_a=a.Reserve(Producer::Output,token);});
        std::thread two([&]{start.arrive(); admitted_b=b.Reserve(Producer::Capture,token);});
        start.wait(2); start.release(); one.join(); two.join();
        assert(admitted_a!=admitted_b); assert(a.Snapshot().active+b.Snapshot().active==1);
        auto& winner=admitted_a?a:b; auto& loser=admitted_a?b:a;
        assert(!loser.Complete(token)); assert(!loser.AllowsPublication(token));
        assert(winner.Complete(token)); assert(!winner.Complete(token));
        assert(!loser.Reserve(Producer::Capture,token));
    } else if(name=="close_admission_race") {
        for(unsigned round=0;round<16;++round) {
            Gate gate; const auto original=open(gate); Reservation token; Barrier start;
            bool admitted=false; Generation closing=0;
            std::thread one([&]{start.arrive(); admitted=gate.Reserve(Producer::Capture,token);});
            std::thread two([&]{start.arrive(); closing=gate.BeginClose();});
            start.wait(2); start.release(); one.join(); two.join();
            assert(closing==original+1); assert(!gate.AllowsPublication(token));
            assert(gate.Snapshot().active==(admitted?1u:0u)); acknowledge(gate,closing);
            if(admitted) { assert(!gate.OpenAfterTerminal(owner(),closing)); assert(gate.Complete(token)); }
            assert(gate.OpenAfterTerminal(owner(),closing));
            const auto old=gate.BeginClose(); Barrier next;
            std::thread ack([&]{next.arrive(); gate.Acknowledge(Acknowledgement::Boot,old);});
            std::thread invalidate([&]{next.arrive(); assert(gate.Invalidate()==old+1);});
            next.wait(2); next.release(); ack.join(); invalidate.join();
            assert(gate.Snapshot().acknowledgements==0);
            assert(!gate.OpenAfterTerminal(owner(),old));
        }
    } else if(name=="old_owner_cas") {
        Gate gate; const auto first=open(gate); Reservation output;
        assert(gate.Reserve(Producer::Output,output)); const auto closing=gate.BeginClose();
        Barrier start; bool held=false,old_opened=true;
        std::thread one([&]{start.arrive(); held=gate.Hold(owner(8,4),closing);});
        std::thread two([&]{start.arrive(); old_opened=gate.OpenAfterTerminal(owner(),first);});
        start.wait(2); start.release(); one.join(); two.join(); assert(held&&!old_opened);
        acknowledge(gate,closing); assert(!gate.OpenAfterTerminal(owner(),closing));
        assert(!gate.OpenAfterTerminal(owner(8,4),closing)); assert(gate.Complete(output));
        assert(gate.OpenAfterTerminal(owner(8,4),closing));
    } else if(name=="timer_recovery") {
        for(bool alarm:{false,true}) {
            Gate gate; const auto generation=gate.BeginClose(); assert(gate.Hold(owner(),generation));
            Reservation cleanup,other; const auto identity=timer(alarm);
            assert(!gate.Reserve(Producer::TimerRecovery,cleanup));
            assert(!gate.ReserveTimerRecovery(identity,cleanup));
            assert(gate.RegisterRetainedTimer(identity,generation));
            assert(gate.RegisterRetainedTimer(identity,generation));
            assert(!gate.RegisterRetainedTimer(timer(alarm,23),generation));
            if(alarm) {
                std::vector<TimerIdentity> changed;
                for(auto field : {&TimerIdentity::playback_id,&TimerIdentity::timer_id}) {
                    auto different=identity; (different.*field)[0]=1; changed.push_back(different);
                }
                auto different=identity; ++different.timer_revision; changed.push_back(different);
                different=identity; ++different.attempt; changed.push_back(different);
                changed.push_back(timer(false));
                for(const auto& value:changed) {
                    assert(!gate.RegisterRetainedTimer(value,generation));
                    assert(!gate.ReserveTimerRecovery(value,other));
                    assert(!gate.RetireRetainedTimer(value,generation));
                }
            }
            assert(!gate.ReserveTimerRecovery(timer(alarm,23),cleanup));
            assert(gate.ReserveTimerRecovery(identity,cleanup));
            assert(gate.ReserveTimerRecovery(identity,cleanup));
            assert(!gate.ReserveTimerRecovery(identity,other));
            assert(!gate.AllowsPublication(cleanup)); acknowledge(gate,generation);
            assert(!gate.RetireRetainedTimer(identity,generation)); assert(gate.Complete(cleanup));
            assert(!gate.OpenAfterTerminal(owner(),generation));
            assert(gate.Snapshot().timer_recovery_retained);
            assert(!gate.RetireRetainedTimer(timer(alarm,23),generation));
            assert(!gate.RetireRetainedTimer(identity,generation-1));
            assert(gate.RetireRetainedTimer(identity,generation));
            assert(!gate.RegisterRetainedTimer(identity,generation)); // Too late for Boot.
            assert(gate.Snapshot().blocked); assert(gate.OpenAfterTerminal(owner(),generation));
        }
    } else if(name=="capture_media") {
        Gate gate; open(gate); Reservation capture,read,prep,encode,worker,upload,other;
        assert(gate.Reserve(Producer::Capture,capture));
        for(auto kind:{Producer::Capture,Producer::OrdinaryOutput,Producer::TimerPreparation,
                       Producer::Notification,Producer::InputRead,Producer::Decode})
            assert(!gate.Reserve(kind,other));
        assert(!gate.ReserveTimerMedia(capture,Producer::Decode,other));
        assert(!gate.ReserveCaptureMedia(capture,Producer::Output,other));
        assert(gate.ReserveCaptureMedia(capture,Producer::InputRead,read));
        assert(gate.ReserveCaptureMedia(capture,Producer::InputPreparation,prep));
        assert(gate.ReserveCaptureMedia(capture,Producer::Encode,encode));
        assert(gate.ReserveCaptureMedia(capture,Producer::CaptureWork,worker));
        assert(gate.SealCaptureInput(capture));
        assert(!gate.AllowsPublication(read)&&!gate.AllowsPublication(prep));
        assert(gate.AllowsPublication(encode)&&gate.AllowsPublication(worker));
        assert(!gate.ReserveCaptureMedia(capture,Producer::InputRead,other));
        assert(gate.ReserveCaptureMedia(capture,Producer::CaptureUpload,upload));
        assert(gate.Snapshot(capture).children==5&&gate.Snapshot(capture).input_sealed);
        assert(!gate.CompleteChild(other,read)&&!gate.CompleteChild(capture,capture));
        assert(gate.IsChild(capture,read)&&!gate.IsChild(other,read));
        gate.BeginClose(); assert(!gate.AllowsPublication(upload));
        for(auto* token:{&read,&prep,&encode,&worker,&upload})
            assert(gate.CompleteChild(capture,*token));
        assert(gate.Snapshot(capture).children==0); assert(gate.Complete(capture));
        assert(!gate.Snapshot(capture).owned&&!gate.CompleteChild(capture,read));
    } else if(name=="ordinary_parent") {
        Gate gate; open(gate); Reservation ordinary,decode,output,other;
        assert(gate.Reserve(Producer::OrdinaryOutput,ordinary));
        assert(!gate.Reserve(Producer::Capture,other));
        assert(!gate.Reserve(Producer::Notification,other));
        assert(!gate.Reserve(Producer::TimerPreparation,other));
        assert(!gate.ReserveCaptureMedia(ordinary,Producer::InputRead,other));
        assert(!gate.ReserveTimerMedia(ordinary,Producer::Output,other));
        assert(gate.ReserveMedia(ordinary,Producer::Decode,decode));
        assert(gate.ReserveMedia(ordinary,Producer::Output,output));
        assert(gate.AllowsPublication(output,Producer::Output));
        assert(!gate.AllowsPublication(output,Producer::InputRead));
        assert(gate.Complete(ordinary)); assert(!gate.AllowsPublication(output));
        assert(!gate.Reserve(Producer::Capture,other));
        assert(gate.Complete(output)&&gate.Complete(decode));
        assert(gate.Reserve(Producer::Capture,other));
    } else if(name=="timer_media") {
        Gate gate; open(gate); Reservation capture,parent,wrong,decode,output,late;
        assert(gate.Reserve(Producer::Capture,capture));
        assert(!gate.Reserve(Producer::TimerPreparation,parent));
        assert(gate.Complete(capture)); assert(gate.Reserve(Producer::TimerPreparation,parent));
        for(unsigned i=0;i<8;++i) assert(!gate.Reserve(static_cast<Producer>(i),wrong));
        assert(!gate.ReserveTimerMedia(wrong,Producer::Output,output));
        assert(!gate.ReserveTimerMedia(parent,Producer::Capture,wrong));
        assert(!gate.ReserveTimerMedia(parent,Producer::Decode,parent));
        assert(gate.ReserveTimerMedia(parent,Producer::Decode,decode));
        assert(gate.ReserveTimerMedia(parent,Producer::Output,output));
        assert(gate.AllowsPublication(decode)&&gate.AllowsPublication(output));
        assert(gate.Complete(parent)); assert(!gate.AllowsPublication(output));
        assert(!gate.ReserveTimerMedia(parent,Producer::Output,late));
        assert(!gate.Reserve(Producer::TimerPreparation,wrong));
        assert(gate.Complete(decode)&&gate.Complete(output));
        assert(gate.Reserve(Producer::TimerPreparation,wrong));
        assert(!gate.ReserveTimerMedia(parent,Producer::Output,late));
        const auto generation=gate.BeginClose(); acknowledge(gate,generation);
        assert(!gate.OpenAfterTerminal(owner(),generation)); assert(gate.Complete(wrong));
        assert(gate.OpenAfterTerminal(owner(),generation));
    } else if(name=="timer_prepare_boundary") {
        Gate gate; open(gate); Reservation preparation,cleanup;
        assert(gate.Reserve(Producer::TimerPreparation,preparation));
        const auto generation=gate.BeginClose();
        assert(gate.RegisterRetainedTimer(timer(),generation));
        assert(!gate.ReserveTimerRecovery(timer(),preparation)); // Cannot relabel a producer.
        assert(gate.ReserveTimerRecovery(timer(),cleanup)); assert(gate.Complete(cleanup));
        acknowledge(gate,generation); assert(!gate.RetireRetainedTimer(timer(),generation));
        assert(gate.Complete(preparation)); assert(gate.RetireRetainedTimer(timer(),generation));
        assert(!gate.Complete(preparation)); assert(gate.OpenAfterTerminal(owner(),generation));
        const auto next=gate.BeginClose(); assert(gate.RegisterRetainedTimer(timer(false,24),next));
        assert(!gate.RetireRetainedTimer(timer(),next));
        auto invalid=timer(); invalid.playback_id=uuid(9);
        assert(!gate.RegisterRetainedTimer(invalid,next));
    } else if(name=="exhaustion") {
        Gate invalid(0); assert(invalid.Snapshot().faulted);
        assert(!invalid.Hold(owner(),0)); assert(!invalid.OpenAfterTerminal(owner(),0));
        Gate gate(std::numeric_limits<Generation>::max()-1); open(gate); Reservation token;
        assert(gate.Reserve(Producer::InputRead,token));
        const auto last=gate.BeginClose(); assert(last==std::numeric_limits<Generation>::max());
        for(int i=0;i<10;++i) assert(gate.BeginClose()==last);
        assert(!gate.Snapshot().faulted); acknowledge(gate,last);
        assert(gate.Invalidate()==last); assert(gate.Snapshot().faulted);
        assert(gate.Complete(token)); assert(!gate.Acknowledge(Acknowledgement::Boot,last));
        assert(!gate.OpenAfterTerminal(owner(),last)); assert(gate.Snapshot().blocked);
    } else { assert(false); }
}
'''


class AudioAdmissionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="orbit-audio-admission-")
        cls.addClassCleanup(cls.temp.cleanup)
        path = Path(cls.temp.name)
        (path / "test.cc").write_text(PROGRAM)
        cls.binary = path / "test"
        subprocess.run([
            "c++", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", str(ROOT / "main"),
            str(path / "test.cc"), str(ROOT / "main/audio/provisions_audio_admission.cc"),
            "-o", str(cls.binary),
        ], check=True, capture_output=True, text=True, timeout=60)

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True,
                                timeout=15, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_empty_or_missing_owner_never_opens(self): self.run_case("initial")
    def test_every_explicit_current_generation_ack_is_required(self): self.run_case("every_ack")
    def test_close_retries_are_idempotent_but_new_boundary_invalidates_acks(self): self.run_case("idempotent_close")
    def test_all_producer_tokens_survive_close_until_actual_completion(self): self.run_case("all_producers")
    def test_exact_full_identity_and_monotonic_owner_cas(self): self.run_case("identity")
    def test_capacity_spent_and_lost_tokens_fail_closed(self): self.run_case("capacity_spent_lost")
    def test_old_completion_cannot_release_a_new_press(self): self.run_case("old_completion_new_press")
    def test_simultaneous_reservations_remain_tracked_through_invalidation(self): self.run_case("simultaneous")
    def test_same_token_cannot_be_claimed_by_two_gates(self): self.run_case("cross_gate")
    def test_close_races_admission_and_old_ack_races_invalidation(self): self.run_case("close_admission_race")
    def test_delayed_old_terminal_cannot_open_new_owner(self): self.run_case("old_owner_cas")
    def test_registered_exact_timer_recovery_never_allows_publication(self): self.run_case("timer_recovery")
    def test_capture_parent_seals_only_input_and_retains_exact_worker_children(self): self.run_case("capture_media")
    def test_ordinary_parent_excludes_capture_and_cannot_be_borrowed(self): self.run_case("ordinary_parent")
    def test_timer_preparation_excludes_fresh_work_and_only_exact_parent_admits_media(self): self.run_case("timer_media")
    def test_fresh_preparation_cannot_be_relabelled_as_recovery(self): self.run_case("timer_prepare_boundary")
    def test_generation_exhaustion_never_wraps_or_opens(self): self.run_case("exhaustion")


if __name__ == "__main__":
    unittest.main()
