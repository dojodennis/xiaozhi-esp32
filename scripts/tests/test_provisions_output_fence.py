"""Actual closed wire/core/binary NVS methods; physical hooks remain unbound."""
import hashlib
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = Path(__file__).parent / "fixtures/output_fence_v1.json"

NVS = r'''
#pragma once
#include <cstddef>
#include <cstdint>
using nvs_handle_t = int;
constexpr int ESP_OK=0,ESP_ERR_NVS_NOT_FOUND=1,NVS_READONLY=0,NVS_READWRITE=1;
int nvs_open(const char*,int,nvs_handle_t*);
int nvs_get_blob(nvs_handle_t,const char*,void*,size_t*);
int nvs_set_blob(nvs_handle_t,const char*,const void*,size_t);
int nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
'''
PROGRAM = r'''
#include "provisions_output_fence.h"
#include "nvs.h"
#include <cJSON.h>
#include <cassert>
#include <cstring>
#include <iostream>
#include <map>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>
using namespace provisions::output_fence;
__MESSAGES__
std::vector<uint8_t> committed,staged;
std::map<std::string,std::string> other_namespaces{{"orbit_dct_v1","occupied dictation"},{"orbit_tmr_v1","occupied timer"},{"voice","occupied raw audio"},{"settings","occupied settings"}};
enum Fault {None,ReadOpen,WriteOpen,ReadSize,ReadData,Set,Commit,UncertainCommit,Readback};
Fault fault=None;bool did_write=false;int writes=0,opens=0;
int nvs_open(const char* ns,int mode,nvs_handle_t* handle){assert(std::string(ns)=="orbit_of_v1");if((mode==NVS_READONLY&&fault==ReadOpen)||(mode==NVS_READWRITE&&fault==WriteOpen))return 2;*handle=mode;if(mode==NVS_READONLY&&committed.empty())return ESP_ERR_NVS_NOT_FOUND;return 0;}
int nvs_get_blob(nvs_handle_t,const char* key,void* bytes,size_t* size){assert(std::string(key)=="owner");if(fault==Readback&&did_write)return 2;if((!bytes&&fault==ReadSize)||(bytes&&fault==ReadData))return 2;if(committed.empty())return ESP_ERR_NVS_NOT_FOUND;if(!bytes){*size=committed.size();return 0;}if(*size<committed.size())return 2;*size=committed.size();std::memcpy(bytes,committed.data(),*size);return 0;}
int nvs_set_blob(nvs_handle_t,const char* key,const void* bytes,size_t size){assert(std::string(key)=="owner");assert(size==224);if(fault==Set)return 2;auto p=static_cast<const uint8_t*>(bytes);staged.assign(p,p+size);return 0;}
int nvs_commit(nvs_handle_t){if(fault==Commit)return 2;committed=staged;did_write=true;++writes;return fault==UncertainCommit?2:0;}
void nvs_close(nvs_handle_t){}
Message parse(const std::string& text){Message m;assert(ParseMessage(text,m));return m;}
Record read(){NvsStore s;Record r;assert(s.Load(r)==Store::LoadResult::Present);return r;}
Record fixture_record(Phase phase){Record r;r.identity=parse(acquire).identity;r.phase=phase;if(phase!=Phase::Owned)r.receipt=parse(release_message).receipt;if(phase==Phase::Terminal)r.backend_commit_id=parse(close_commit).backend_commit_id;return r;}
void seed(Phase phase){Manifest b;assert(EncodeRecord(fixture_record(phase),b));committed.assign(b.begin(),b.end());staged.clear();fault=None;did_write=false;writes=0;opens=0;}
std::string change(std::string text,const char* key,const std::string& value){auto r=cJSON_Parse(text.c_str());assert(r);cJSON_ReplaceItemInObjectCaseSensitive(r,key,cJSON_CreateString(value.c_str()));char* out=cJSON_PrintUnformatted(r);text=out;cJSON_free(out);cJSON_Delete(r);return text;}
std::string number(std::string text,const char* key,double value){auto r=cJSON_Parse(text.c_str());assert(r);cJSON_ReplaceItemInObjectCaseSensitive(r,key,cJSON_CreateNumber(value));char* out=cJSON_PrintUnformatted(r);text=out;cJSON_free(out);cJSON_Delete(r);return text;}
std::string newer(std::string text,bool new_lease=false){text=number(text,"fence_epoch",2);text=number(text,"sequence",new_lease?1:2);text=change(text,"checkpoint_sha256",std::string(64,'a'));text=change(text,"playback_id","cccccccc-cccc-4ccc-8ccc-cccccccccccc");if(new_lease)text=change(text,"lease_id","dddddddd-dddd-4ddd-8ddd-dddddddddddd");return text;}
struct Hardware:Physical {
 bool blocked=false,have_owner=false,input=true,output=true,fallback=true,hold_ok=true,open_ok=true,gate_evidence=true,open_uncertain=false;
 std::function<void()> during_hold;
 Identity owner;
 void BlockAll()override{blocked=true;}
 bool Hold(const Identity& id)override{assert(blocked);if(!hold_ok)return false;Record current;NvsStore store;assert(store.Load(current)==Store::LoadResult::Present);assert(SameIdentity(current.identity,id));owner=id;have_owner=true;if(during_hold){auto run=std::move(during_hold);run();}return true;}
 Evidence Observe(const Identity& id)override{return {blocked&&gate_evidence&&have_owner&&SameIdentity(owner,id),input,output,fallback};}
 bool OpenAfterTerminal(const Identity& id)override{assert(blocked&&have_owner&&SameIdentity(owner,id));assert(read().phase==Phase::Terminal&&SameIdentity(read().identity,id));if(!open_ok){if(open_uncertain)blocked=false;return false;}blocked=false;++opens;return true;}
 bool TryCapture()const{return !blocked;}
 bool TryOutput()const{return !blocked;}
};
const auto device=parse(acquire).identity.device_id;
void expect(Core& core,const std::string& text,Result result){auto reply=core.Handle(text);assert(reply.result==result);assert(reply.json.empty()==(result==Result::Denied||result==Result::RecoveryRequired));}
int main(int argc,char** argv){assert(argc==2);std::string mode=argv[1];auto foreign=other_namespaces;
 if(mode=="wire"){
  assert(parse(acquire).command==Command::Acquire);assert(parse(release_message).command==Command::Release);assert(parse(close_commit).command==Command::CloseCommit);
  for(auto result:{Result::Acquired,Result::DrainPending,Result::Released})std::cout<<ReplyJson(result,parse(acquire).identity)<<"\n";
 }else if(mode=="stdin"){
  std::string line;while(std::getline(std::cin,line)){Message m;std::cout<<(ParseMessage(line,m)?"valid":"invalid")<<"\n";}
 }else if(mode=="lifecycle"){
  seed(Phase::Owned);NvsStore s;Hardware h;Core core(s,h,device);assert(!core.GateOpen()&&h.blocked);assert(core.Hydrate());expect(core,acquire,Result::Acquired);assert(writes==0);
  expect(core,close_commit,Result::Denied);assert(h.blocked);expect(core,release_message,Result::DrainPending);assert(read().phase==Phase::DrainedPendingCommit&&writes==1);assert(!core.GateOpen());
  expect(core,acquire,Result::Denied);expect(core,release_message,Result::DrainPending);assert(writes==1);expect(core,number(release_message,"drained_at_ms",1981),Result::Denied);
  expect(core,close_commit,Result::Released);assert(core.GateOpen()&&writes==2&&opens==1);expect(core,close_commit,Result::Released);assert(writes==2&&opens==1);
  expect(core,acquire,Result::Denied);expect(core,release_message,Result::Denied);expect(core,change(close_commit,"backend_commit_id","cccccccc-cccc-4ccc-8ccc-cccccccccccc"),Result::Denied);
  expect(core,newer(acquire),Result::Acquired);assert(writes==3&&!core.GateOpen()&&!h.TryCapture()&&!h.TryOutput());expect(core,close_commit,Result::Denied);expect(core,release_message,Result::Denied);assert(read().phase==Phase::Owned&&read().identity.fence_epoch==2);
  expect(core,newer(acquire),Result::Acquired);assert(writes==3);expect(core,change(newer(acquire),"device_connection_id","dddddddd-dddd-4ddd-8ddd-dddddddddddd"),Result::Denied);
 }else if(mode=="new_lease"){
  seed(Phase::Terminal);NvsStore s;Hardware h;Core core(s,h,device);assert(core.Hydrate()&&core.GateOpen());expect(core,number(newer(acquire),"sequence",1),Result::Denied);expect(core,newer(acquire,true),Result::Acquired);assert(read().identity.sequence==1);
 }else if(mode=="unknown_physical"){
  for(int field=0;field<5;++field){seed(Phase::Owned);NvsStore s;Hardware h;Core c(s,h,device);assert(c.Hydrate());if(field==0)h.input=false;if(field==1)h.output=false;if(field==2)h.fallback=false;if(field==3)h.hold_ok=false;if(field==4)h.gate_evidence=false;
   expect(c,acquire,Result::RecoveryRequired);expect(c,release_message,Result::RecoveryRequired);assert(!h.TryCapture()&&!h.TryOutput()&&writes==0&&read().phase==Phase::Owned);}
 }else if(mode=="reboot"){
  for(auto phase:{Phase::Owned,Phase::DrainedPendingCommit,Phase::Terminal}){seed(phase);NvsStore s;Hardware h;Core c(s,h,device);assert(c.Hydrate());assert(c.GateOpen()==(phase==Phase::Terminal));if(phase==Phase::Owned)expect(c,acquire,Result::Acquired);else expect(c,acquire,Result::Denied);if(phase==Phase::DrainedPendingCommit){expect(c,release_message,Result::DrainPending);expect(c,close_commit,Result::Released);}if(phase==Phase::Terminal)expect(c,close_commit,Result::Released);}
  seed(Phase::Terminal);NvsStore s;Hardware h;h.output=false;Core c(s,h,device);assert(!c.Hydrate()&&!c.GateOpen());expect(c,close_commit,Result::RecoveryRequired);h.output=true;expect(c,close_commit,Result::Released);
 }else if(mode=="missing_corrupt"){
  committed.clear();NvsStore s;Hardware h;Core c(s,h,device);assert(!c.Hydrate());expect(c,acquire,Result::RecoveryRequired);expect(c,release_message,Result::RecoveryRequired);expect(c,close_commit,Result::RecoveryRequired);assert(committed.empty()&&h.blocked&&writes==0);
  seed(Phase::Owned);auto good=committed;for(size_t i=0;i<224;++i){committed=good;committed[i]^=1;Hardware hw;Core bad(s,hw,device);assert(!bad.Hydrate()&&hw.blocked);expect(bad,acquire,Result::RecoveryRequired);}committed=good;committed.push_back(0);Hardware hw;Core bad(s,hw,device);assert(!bad.Hydrate());
 }else if(mode=="read_faults"){
  for(auto f:{ReadOpen,ReadSize,ReadData}){seed(Phase::Terminal);fault=f;NvsStore s;Hardware h;Core c(s,h,device);assert(!c.Hydrate()&&!c.GateOpen()&&h.blocked);expect(c,acquire,Result::RecoveryRequired);assert(writes==0);}
 }else if(mode=="write_faults"){
  for(auto phase:{Phase::Owned,Phase::DrainedPendingCommit,Phase::Terminal})for(auto f:{WriteOpen,Set,Commit,UncertainCommit,Readback}){
   seed(phase);NvsStore s;Hardware h;Core c(s,h,device);assert(c.Hydrate());auto command=phase==Phase::Owned?release_message:phase==Phase::DrainedPendingCommit?close_commit:newer(acquire);fault=f;
   expect(c,command,Result::RecoveryRequired);assert(!c.GateOpen()&&h.blocked);expect(c,command,Result::RecoveryRequired);fault=None;
   auto actual=read().phase;Hardware reboot;Core next(s,reboot,device);assert(next.Hydrate());assert(next.GateOpen()==(actual==Phase::Terminal));
   // A failed readback can leave a durable newer owner or pending drain. Reboot must retain it.
   if((f==Readback||f==UncertainCommit)&&phase==Phase::Terminal){assert(actual==Phase::Owned);expect(next,newer(acquire),Result::Acquired);assert(!next.GateOpen());}
   if((f==Readback||f==UncertainCommit)&&phase==Phase::Owned){assert(actual==Phase::DrainedPendingCommit);expect(next,acquire,Result::Denied);expect(next,release_message,Result::DrainPending);assert(!next.GateOpen());}
  }
 }else if(mode=="open_failure"){
  seed(Phase::DrainedPendingCommit);NvsStore s;Hardware h;Core c(s,h,device);assert(c.Hydrate());h.open_ok=false;h.open_uncertain=true;expect(c,close_commit,Result::RecoveryRequired);assert(read().phase==Phase::Terminal&&h.blocked);expect(c,acquire,Result::Denied);h.open_ok=true;expect(c,close_commit,Result::Released);assert(writes==1);
 }else if(mode=="wrong_identity"){
  for(auto command:{acquire,release_message,close_commit})for(auto key:{"device_id","lease_id","playback_id"}){seed(Phase::Owned);NvsStore s;Hardware h;Core c(s,h,device);assert(c.Hydrate());expect(c,change(command,key,"dddddddd-dddd-4ddd-8ddd-dddddddddddd"),Result::Denied);assert(h.blocked&&writes==0);}
  seed(Phase::Owned);NvsStore s;Hardware h;Core other(s,h,"dddddddd-dddd-4ddd-8ddd-dddddddddddd");assert(!other.Hydrate()&&h.blocked);
 }else if(mode=="cas"){
  seed(Phase::Owned);NvsStore s;auto old=read();auto pending=old;pending.phase=Phase::DrainedPendingCommit;pending.receipt=parse(release_message).receipt;assert(s.CompareExchange(old,pending));assert(!s.CompareExchange(old,pending));assert(read().phase==Phase::DrainedPendingCommit);assert(!s.CompareExchange(pending,old));auto skipped=old;skipped.phase=Phase::Terminal;skipped.receipt=pending.receipt;skipped.backend_commit_id=parse(close_commit).backend_commit_id;seed(Phase::Owned);assert(!s.CompareExchange(old,skipped));
  std::atomic<int> accepted{0};std::thread one([&]{if(s.CompareExchange(old,pending))++accepted;});NvsStore second;std::thread two([&]{if(second.CompareExchange(old,pending))++accepted;});one.join();two.join();assert(accepted==1&&read().phase==Phase::DrainedPendingCommit);
 }else if(mode=="old_commit_race"){
  seed(Phase::Terminal);NvsStore s;Hardware h;Core c(s,h,device);assert(c.Hydrate());std::thread late;Result result=Result::RecoveryRequired;
  h.during_hold=[&]{assert(read().identity.fence_epoch==2);late=std::thread([&]{result=c.Handle(close_commit).result;});};
  expect(c,newer(acquire),Result::Acquired);late.join();assert(result==Result::Denied&&h.blocked&&!c.GateOpen()&&read().identity.fence_epoch==2);
 }else if(mode=="no_start"){
  seed(Phase::Owned);auto root=cJSON_Parse(release_message.c_str());cJSON_ReplaceItemInObjectCaseSensitive(root,"completed",cJSON_CreateFalse());cJSON_ReplaceItemInObjectCaseSensitive(root,"started_at_ms",cJSON_CreateNull());char* text=cJSON_PrintUnformatted(root);NvsStore s;Hardware h;Core c(s,h,device);assert(c.Hydrate());expect(c,text,Result::DrainPending);assert(!read().receipt.has_started&&!read().receipt.completed);cJSON_free(text);cJSON_Delete(root);
 }else assert(false);
 assert(foreign==other_namespaces);
}
'''


class OutputFenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture = json.loads(FIXTURE.read_text())
        cls.temp = tempfile.TemporaryDirectory(prefix="orbit-output-fence-test-")
        cls.addClassCleanup(cls.temp.cleanup)
        path = Path(cls.temp.name)
        (path / "nvs.h").write_text(NVS)
        messages = cls.fixture["messages"]
        literals = "\n".join(f'const std::string {name}=R"wire({json.dumps(messages[key], separators=(",", ":"))})wire";'
                             for name, key in [("acquire", "acquire"), ("release_message", "release"), ("close_commit", "close_commit")])
        (path / "test.cc").write_text(PROGRAM.replace("__MESSAGES__", literals))
        cjson = ROOT / "managed_components/espressif__cjson/cJSON"
        cls.binary = path / "test"
        command = ["c++", "-std=c++17", "-pthread", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                   "-I", str(path), "-I", str(ROOT / "main"), "-I", str(cjson), str(path / "test.cc"),
                   *(str(ROOT / "main" / name) for name in ["provisions_output_fence.cc", "provisions_output_fence_wire.cc", "provisions_output_fence_store.cc"]),
                   ]
        # Compile C separately: the sanitizer applies to parser and C++ handlers alike.
        subprocess.run(["cc", "-fsanitize=address,undefined", "-I", str(cjson), "-c", str(cjson / "cJSON.c"), "-o", str(path / "cJSON.o")],
                       check=True, capture_output=True, text=True, timeout=60)
        subprocess.run(command + [str(path / "cJSON.o"), "-o", str(cls.binary)],
                       check=True, capture_output=True, text=True, timeout=60)

    def run_case(self, name, input_text=None):
        result = subprocess.run([str(self.binary), name], input=input_text, text=True, capture_output=True,
                                timeout=15, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def test_wire_shared_fixture(self):
        self.assertEqual(hashlib.sha256(self.fixture["canonical_json"].encode()).hexdigest(),
                         "6b93cb0e6e4e074d494685a44b4b656286edf5f97d85fa2e10b2ba1e4c228903")
        replies = [json.loads(line) for line in self.run_case("wire").splitlines()]
        self.assertEqual(replies, [self.fixture["messages"][key] for key in ["acquired", "drain_pending", "released"]])

    def test_wire_rejects_open_and_ambiguous_input(self):
        invalid = []
        for kind in ["acquire", "release", "close_commit"]:
            original = self.fixture["messages"][kind]
            for key in original:
                missing = dict(original); del missing[key]; invalid.append(json.dumps(missing))
            for key, value in [("extra", 1), ("version", 2), ("device_id", "00000000-0000-0000-0000-000000000000"),
                               ("lease_id", "AAAAAAAA-AAAA-4AAA-8AAA-AAAAAAAAAAAA"), ("fence_epoch", 0), ("fence_epoch", 9007199254740992),
                               ("sequence", True), ("checkpoint_sha256", "A" * 64)]:
                changed = {**original, key: value}; invalid.append(json.dumps(changed))
            text = json.dumps(original, separators=(",", ":"))
            invalid.extend([text[:-1] + ',"version":1}', text + '{}', text.replace('"version":1', '"version":1.0'),
                            text.replace('"version":1', '"version":1e0'), text.replace('"version":1', '"version":01'),
                            text.replace('"device_id":"', '"device_id":"\\u0000'), ' ' * 2049 + text, '[' * 100 + text + ']' * 100])
        release = self.fixture["messages"]["release"]
        for patch in [{"started_at_ms": None}, {"started_at_ms": 1481}, {"drained_at_ms": 1979}, {"completed": 1},
                      {"stopped_at_ms": -1}, {"drained_at_ms": 9007199254740992}]:
            invalid.append(json.dumps({**release, **patch}))
        self.assertEqual(self.run_case("stdin", "\n".join(invalid) + "\n").splitlines(), ["invalid"] * len(invalid))

    def test_lifecycle_exact_phase_retries(self): self.run_case("lifecycle")
    def test_sequence_is_per_lease(self): self.run_case("new_lease")
    def test_unknown_physical_hooks_do_not_acknowledge(self): self.run_case("unknown_physical")
    def test_reboot_all_durable_phases(self): self.run_case("reboot")
    def test_missing_corrupt_record_requires_recovery(self): self.run_case("missing_corrupt")
    def test_actual_nvs_read_faults(self): self.run_case("read_faults")
    def test_actual_nvs_write_commit_readback_faults(self): self.run_case("write_faults")
    def test_terminal_gate_open_failure_retries_exactly(self): self.run_case("open_failure")
    def test_wrong_device_or_attempt_does_not_mutate(self): self.run_case("wrong_identity")
    def test_actual_nvs_compare_exchange(self): self.run_case("cas")
    def test_old_close_commit_races_new_owner(self): self.run_case("old_commit_race")
    def test_never_started_terminal_receipt(self): self.run_case("no_start")


if __name__ == "__main__":
    unittest.main()
