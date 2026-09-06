"""Compile the actual bounded PCM core and exercise buffer/press ownership."""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PROGRAM = r'''
#include "provisions_voice_recording.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>
using namespace provisions;
struct Storage {
    // Guard samples verify both ends of each caller-owned recording buffer.
    std::vector<int16_t> a=std::vector<int16_t>(VoiceRecording::kMaxSamples+2,0x1234);
    std::vector<int16_t> b=std::vector<int16_t>(VoiceRecording::kMaxSamples+2,0x1234);
    int16_t* first(){return a.data()+1;}
    int16_t* second(){return b.data()+1;}
    void guards() const {assert(a.front()==0x1234 && a.back()==0x1234 && b.front()==0x1234 && b.back()==0x1234);}
};
VoiceContext context(int conversation=1,int source=2,uint32_t revision=3) {
    VoiceContext c;c.conversation_id[0]=conversation;c.source_request_id[0]=source;c.source_revision=revision;return c;
}
std::array<int16_t,160> mono(int16_t value=41){std::array<int16_t,160> a;a.fill(value);return a;}
void done(VoiceRecording& core,const VoiceRecording::Work& work) {
    // The real worker clears the memory while it still owns Processing.
    memset(const_cast<int16_t*>(work.pcm),0,VoiceRecording::kMaxSamples*sizeof(int16_t));core.Finish(work);
}
int main() {
    Storage memory;VoiceRecording core(memory.first(),memory.second(),VoiceRecording::kMaxSamples);
    auto samples=mono();VoiceRecording::Work first,second,other;
    assert(!core.Take(other) && other.pcm==nullptr && other.slot==VoiceRecording::kBufferCount);
    auto frozen=context();assert(core.Begin(1,frozen,1788679000123ULL));
    frozen=context(8,9,10); // Caller topic changes cannot rewrite the captured source.
    assert(core.Append(1,samples.data(),samples.size(),1));core.Release(1);
    assert(core.Begin(2,frozen,0));assert(core.Append(2,samples.data(),samples.size(),1));
    // Old duplicate releases/appends/failures have no authority over the new microphone.
    core.Release(1);core.Fail(1);assert(!core.Append(1,samples.data(),samples.size(),1));
    assert(core.IsRecording(2));assert(!core.Begin(2,frozen,0));
    assert(!core.Begin(3,frozen,0)); // A third press cannot evict either live recording.
    assert(core.Take(first));assert(first.press==1 && first.samples==160 && !first.failed);
    assert(first.capture.conversation_id==context().conversation_id);
    assert(first.capture.source_request_id==context().source_request_id && first.capture.source_revision==3);
    assert(first.capture.captured_unix_ms==1788679000123ULL);
    assert(first.capture.request_id==VoiceId{} && first.capture.packet_count==0);
    auto before=std::vector<int16_t>(first.pcm,first.pcm+first.samples);
    core.Release(2);assert(core.Take(second));assert(second.press==2 && second.pcm!=first.pcm);
    assert(!core.Begin(3,frozen,0));assert(!core.Take(other));
    assert(std::equal(before.begin(),before.end(),first.pcm));
    assert(second.capture.conversation_id==frozen.conversation_id && second.capture.source_revision==10);
    auto forged=first;forged.press=99;core.Finish(forged);assert(!core.Begin(3,frozen,0));
    forged=first;forged.pcm=second.pcm;core.Finish(forged);assert(!core.Begin(3,frozen,0));
    done(core,first);assert(core.Begin(3,context(),0));core.Finish(first);
    assert(core.IsRecording(3));assert(core.Append(3,samples.data(),samples.size(),1));core.Release(3);
    assert(core.Take(other));assert(other.press==3 && other.pcm==first.pcm && !other.failed);
    assert(!core.Begin(1,context(),0) && !core.Begin(2,context(),0));
    done(core,other);done(core,second);memory.guards();

    // Ten seconds means exactly 160000 samples, without truncating overflow into success.
    Storage bounded;VoiceRecording limit(bounded.first(),bounded.second(),VoiceRecording::kMaxSamples);
    assert(limit.Begin(1,context(),0));
    for(size_t i=0;i<1000;i++)assert(limit.Append(1,samples.data(),160,1));
    assert(limit.IsRecording(1));limit.Release(1);assert(limit.Take(first));
    assert(first.samples==160000 && !first.failed && first.pcm[159999]==41);done(limit,first);
    assert(limit.Begin(2,context(),0));
    for(size_t i=0;i<1000;i++)assert(limit.Append(2,samples.data(),160,1));
    assert(!limit.Append(2,samples.data(),1,1));assert(!limit.IsRecording(2));
    limit.Release(2);assert(limit.Take(first) && first.failed && first.samples==160000);
    done(limit,first);bounded.guards();

    // Invalid chunks fail only their own recording. Stereo keeps microphone left.
    for(int bad=0;bad<6;bad++) {
        Storage s;VoiceRecording r(s.first(),s.second(),VoiceRecording::kMaxSamples);
        assert(r.Begin(1,context(),0));assert(r.Append(1,samples.data(),160,1));
        assert(!r.Append(1,bad==0?nullptr:samples.data(),bad==1?0:bad==2?161:160,
                         bad==3?0:bad==4?3:bad==5?SIZE_MAX:1));
        assert(!r.Append(1,samples.data(),160,1));r.Release(1);assert(r.Take(first));
        assert(first.failed && first.samples==160);done(r,first);s.guards();
    }
    Storage stereo;VoiceRecording pair(stereo.first(),stereo.second(),VoiceRecording::kMaxSamples);
    std::array<int16_t,320> channels;
    for(size_t i=0;i<160;i++){channels[i*2]=int16_t(i-80);channels[i*2+1]=30000;}
    assert(pair.Begin(1,context(),0));assert(pair.Append(1,channels.data(),160,2));pair.Release(1);
    assert(pair.Take(first) && !first.failed);
    for(size_t i=0;i<160;i++)assert(first.pcm[i]==int16_t(i-80));done(pair,first);
    assert(pair.Begin(2,context(),0));pair.Release(2);assert(pair.Take(first)&&first.failed&&first.samples==0);done(pair,first);

    // Context and capacity validation never publishes a partially initialized work item.
    Storage invalid;
    for(int variant=0;variant<4;variant++) {
        VoiceRecording bad(variant==0?nullptr:invalid.first(),variant==1?nullptr:variant==2?invalid.first():invalid.second(),
                           variant==3?VoiceRecording::kMaxSamples-1:VoiceRecording::kMaxSamples);
        assert(!bad.Begin(1,context(),0));assert(!bad.Take(first));
    }
    VoiceRecording valid(invalid.first(),invalid.second(),VoiceRecording::kMaxSamples);
    assert(!valid.Begin(0,context(),0));assert(!valid.Begin(1,context(0),0));
    assert(!valid.Begin(1,context(1,0,1),0));assert(!valid.Begin(1,context(1,2,1000000000),0));
    assert(!valid.Begin(1,context(),253402300800000ULL));
    assert(valid.Begin(UINT32_MAX,context(1,0,0),253402300799999ULL));valid.Release(UINT32_MAX);
    assert(valid.Take(first));done(valid,first);assert(!valid.Begin(1,context(),0));

    // Concurrent old callbacks cannot append to, release, or fail a fresh press.
    Storage racing;VoiceRecording race(racing.first(),racing.second(),VoiceRecording::kMaxSamples);
    for(uint32_t press=1;press<=500;press++) {
        assert(race.Begin(press,context(),0));
        std::thread stale([&]{for(int i=0;i<100;i++) {
            assert(!race.Append(press-1,samples.data(),160,1));race.Release(press-1);race.Fail(press-1);
        }});
        for(int i=0;i<10;i++)assert(race.Append(press,samples.data(),160,1));
        stale.join();assert(race.IsRecording(press));race.Release(press);
        assert(race.Take(first) && first.press==press && first.samples==1600 && !first.failed);done(race,first);
    }
    racing.guards();
    std::cout<<"PCM recording ownership, exact bounds, and stale-press cases passed\n";
}
'''


class VoiceRecordingReviewTests(unittest.TestCase):
    def test_actual_recording_core(self):
        compiler = shutil.which("c++")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory(prefix="orbit-recording-review-") as folder:
            root = Path(folder)
            source, binary = root / "review.cc", root / "review"
            source.write_text(PROGRAM)
            subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                            "-I", str(ROOT / "main"), str(source),
                            str(ROOT / "main/provisions_voice_recording.cc"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, env={**os.environ, "ASAN_OPTIONS":
                            "detect_leaks=0" if sys.platform == "darwin" else "detect_leaks=1"})


if __name__ == "__main__":
    unittest.main()
