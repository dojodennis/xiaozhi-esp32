"""Actual synchronized Core snapshot: read-only spies, NVS faults, and transitions."""

import unittest

import test_provisions_output_fence as base

HELPERS = r'''
#include <chrono>
#include <future>
#include <tuple>
struct SnapshotStore:Store {
 NvsStore actual;int loads=0,cas=0;
 LoadResult Load(Record& record)override{++loads;return actual.Load(record);}
 bool CompareExchange(const Record& expected,const Record& desired)override{
  ++cas;return actual.CompareExchange(expected,desired);
 }
};
struct SnapshotHardware:Hardware {
 int blocks=0,holds=0,observes=0,open_calls=0;
 void BlockAll()override{++blocks;Hardware::BlockAll();}
 bool Hold(const Identity& id)override{++holds;return Hardware::Hold(id);}
 Evidence Observe(const Identity& id)override{++observes;return Hardware::Observe(id);}
 bool OpenAfterTerminal(const Identity& id)override{++open_calls;return Hardware::OpenAfterTerminal(id);}
};
void snapshot(Core& core,SnapshotStore& store,SnapshotHardware& hardware,
              Readiness state,std::optional<uint64_t> epoch){
 const auto before=std::make_tuple(store.loads,store.cas,hardware.blocks,hardware.holds,
                                  hardware.observes,hardware.open_calls,writes,committed,staged);
 for(int i=0;i<100;++i){auto value=core.Snapshot();assert(value.state==state&&value.fence_epoch==epoch);
  value.state=Readiness::Blocked;value.fence_epoch=999;}
 assert(before==std::make_tuple(store.loads,store.cas,hardware.blocks,hardware.holds,
                               hardware.observes,hardware.open_calls,writes,committed,staged));
}
void snapshot_seed(Phase phase,Origin origin=Origin::Normal){
 seed(Phase::Owned);auto value=fixture_record(Phase::Owned);value.phase=phase;value.origin=origin;
 if(phase!=Phase::Owned)value.receipt=parse(release_message).receipt;
 if(origin==Origin::AbortUnacquired){value.receipt.completed=false;value.receipt.has_started=false;value.receipt.started_at_ms=0;}
 if(phase==Phase::Terminal)value.backend_commit_id=parse(close_commit).backend_commit_id;
 Manifest bytes;assert(EncodeRecord(value,bytes));committed.assign(bytes.begin(),bytes.end());
}
std::string snapshot_abort(){
 auto text=change(newer(acquire),"type","output_fence_v1.abort_unacquired");auto root=cJSON_Parse(text.c_str());
 cJSON_DeleteItemFromObjectCaseSensitive(root,"mode");cJSON_AddBoolToObject(root,"completed",false);
 cJSON_AddNullToObject(root,"started_at_ms");cJSON_AddNumberToObject(root,"stopped_at_ms",1480);
 cJSON_AddNumberToObject(root,"drained_at_ms",1980);char* raw=cJSON_PrintUnformatted(root);
 text=raw;cJSON_free(raw);cJSON_Delete(root);return text;
}
struct SnapshotAuthority:AbortAuthority {
 Message approved=parse(snapshot_abort());unsigned calls=0;
 bool Allows(const Identity& id,const PhoneReceipt& receipt)override{
  ++calls;return SameIdentity(id,approved.identity)&&SameReceipt(receipt,approved.receipt);
 }
};
'''

CASES = r'''
 if(mode=="snapshot_cold_missing"){
  committed.clear();fault=None;SnapshotStore store;SnapshotHardware h;Core core(store,h,device);
  snapshot(core,store,h,Readiness::Blocked,std::nullopt);assert(store.loads==0&&h.blocks==1);
  assert(!core.Hydrate());snapshot(core,store,h,Readiness::Uncommissioned,std::nullopt);
  expect(core,acquire,Result::RecoveryRequired);snapshot(core,store,h,Readiness::Uncommissioned,std::nullopt);
  seed(Phase::Terminal);snapshot(core,store,h,Readiness::Uncommissioned,std::nullopt);
  assert(core.Hydrate());snapshot(core,store,h,Readiness::Ready,1);assert(core.GateOpen());
  fault=ReadOpen;assert(!core.Hydrate());snapshot(core,store,h,Readiness::Blocked,std::nullopt);
  fault=None;snapshot(core,store,h,Readiness::Blocked,std::nullopt);
  assert(core.Hydrate());snapshot(core,store,h,Readiness::Ready,1);
 }else if(mode=="snapshot_invalid_storage"){
  for(auto id:{std::string("bad"),std::string("00000000-0000-0000-0000-000000000000")}){
   committed.clear();fault=None;SnapshotStore store;SnapshotHardware h;Core core(store,h,id);
   assert(!core.Hydrate());snapshot(core,store,h,Readiness::Blocked,std::nullopt);assert(store.loads==0);
  }
  for(auto f:{ReadOpen,ReadSize,ReadData}){seed(Phase::Terminal);fault=f;SnapshotStore store;SnapshotHardware h;Core core(store,h,device);
   assert(!core.Hydrate());snapshot(core,store,h,Readiness::Blocked,std::nullopt);assert(store.cas==0);}
  seed(Phase::Terminal);auto original=committed;
  for(size_t i=0;i<original.size();++i){committed=original;committed[i]^=1;SnapshotStore store;SnapshotHardware h;Core core(store,h,device);
   assert(!core.Hydrate());snapshot(core,store,h,Readiness::Blocked,std::nullopt);assert(store.cas==0);}
  committed=original;SnapshotStore store;SnapshotHardware h;Core foreign(store,h,"dddddddd-dddd-4ddd-8ddd-dddddddddddd");
  assert(!foreign.Hydrate());snapshot(foreign,store,h,Readiness::Blocked,std::nullopt);
 }else if(mode=="snapshot_reboot_phases"){
  for(auto phase:{Phase::Owned,Phase::DrainedPendingCommit,Phase::Terminal,
                  Phase::AbortUnacquiredUnclosed,Phase::AbortUnacquiredDrainedPendingCommit}){
   auto origin=(phase==Phase::AbortUnacquiredUnclosed||phase==Phase::AbortUnacquiredDrainedPendingCommit)?Origin::AbortUnacquired:Origin::Normal;
   snapshot_seed(phase,origin);SnapshotStore store;SnapshotHardware h;Core core(store,h,device);
   snapshot(core,store,h,Readiness::Blocked,std::nullopt);assert(core.Hydrate());
   snapshot(core,store,h,phase==Phase::Terminal?Readiness::Ready:Readiness::RecoveryRequired,1);
   assert(store.cas==0&&writes==0);
  }
  snapshot_seed(Phase::Terminal,Origin::AbortUnacquired);SnapshotStore store;SnapshotHardware h;Core core(store,h,device);
  assert(core.Hydrate());snapshot(core,store,h,Readiness::Ready,1);
 }else if(mode=="snapshot_physical_failure"){
  for(int field=0;field<5;++field){seed(Phase::Owned);SnapshotStore store;SnapshotHardware h;Core core(store,h,device);assert(core.Hydrate());
   snapshot(core,store,h,Readiness::RecoveryRequired,1);
   if(field==0)h.input=false;if(field==1)h.output=false;if(field==2)h.fallback=false;if(field==3)h.hold_ok=false;if(field==4)h.gate_evidence=false;
   snapshot(core,store,h,Readiness::RecoveryRequired,1); // Reads never refresh hooks.
   expect(core,acquire,Result::RecoveryRequired);snapshot(core,store,h,Readiness::Blocked,1);
   h.input=h.output=h.fallback=h.hold_ok=h.gate_evidence=true;
   snapshot(core,store,h,Readiness::Blocked,1);expect(core,acquire,Result::Acquired);
   snapshot(core,store,h,Readiness::RecoveryRequired,1);
  }
  for(int field=0;field<3;++field){seed(Phase::Terminal);SnapshotStore store;SnapshotHardware h;
   if(field==0)h.hold_ok=false;if(field==1)h.output=false;if(field==2){h.open_ok=false;h.open_uncertain=true;}
   Core core(store,h,device);assert(!core.Hydrate());snapshot(core,store,h,Readiness::Blocked,1);assert(!core.GateOpen());
   h.hold_ok=h.output=h.open_ok=true;snapshot(core,store,h,Readiness::Blocked,1);
   expect(core,close_commit,Result::Released);snapshot(core,store,h,Readiness::Ready,1);assert(writes==0);
  }
 }else if(mode=="snapshot_unknown_commit"){
  for(auto phase:{Phase::Owned,Phase::DrainedPendingCommit,Phase::Terminal})for(auto f:{WriteOpen,Set,Commit,UncertainCommit,Readback}){
   seed(phase);SnapshotStore store;SnapshotHardware h;Core core(store,h,device);assert(core.Hydrate());
   fault=f;auto command=phase==Phase::Owned?release_message:phase==Phase::DrainedPendingCommit?close_commit:newer(acquire);
   expect(core,command,Result::RecoveryRequired);snapshot(core,store,h,Readiness::Blocked,std::nullopt);
   fault=None;snapshot(core,store,h,Readiness::Blocked,std::nullopt);assert(core.Hydrate());
   const auto actual=read();snapshot(core,store,h,actual.phase==Phase::Terminal?Readiness::Ready:Readiness::RecoveryRequired,actual.identity.fence_epoch);
  }
 }else if(mode=="snapshot_lifecycle"){
  seed(Phase::Terminal);SnapshotStore store;SnapshotHardware h;Core core(store,h,device);assert(core.Hydrate());
  snapshot(core,store,h,Readiness::Ready,1);expect(core,"{}",Result::Denied);snapshot(core,store,h,Readiness::Ready,1);
  expect(core,newer(acquire),Result::Acquired);snapshot(core,store,h,Readiness::RecoveryRequired,2);
  expect(core,close_commit,Result::Denied);snapshot(core,store,h,Readiness::RecoveryRequired,2);
  expect(core,newer(release_message),Result::DrainPending);snapshot(core,store,h,Readiness::RecoveryRequired,2);
 expect(core,newer(close_commit),Result::Released);snapshot(core,store,h,Readiness::Ready,2);
 }else if(mode=="snapshot_abort"){
  seed(Phase::Terminal);SnapshotStore store;SnapshotHardware h;SnapshotAuthority auth;Core core(store,h,device,&auth);
  assert(core.Hydrate());snapshot(core,store,h,Readiness::Ready,1);assert(auth.calls==0);
  h.output=false;expect(core,snapshot_abort(),Result::RecoveryRequired);const auto first=auth.calls;
  assert(read().phase==Phase::AbortUnacquiredUnclosed);snapshot(core,store,h,Readiness::Blocked,2);assert(auth.calls==first);
  h.output=true;expect(core,snapshot_abort(),Result::AbortPending);const auto second=auth.calls;
  snapshot(core,store,h,Readiness::RecoveryRequired,2);assert(auth.calls==second);
  expect(core,newer(close_commit),Result::Released);snapshot(core,store,h,Readiness::Ready,2);assert(auth.calls==second);
  expect(core,snapshot_abort(),Result::Denied);snapshot(core,store,h,Readiness::Ready,2);
 }else if(mode=="snapshot_serialization"){
  seed(Phase::Terminal);SnapshotStore store;SnapshotHardware h;Core core(store,h,device);assert(core.Hydrate());
  std::promise<void> held,release;auto gate=release.get_future();
  h.during_hold=[&]{held.set_value();gate.wait();};
  auto writer=std::async(std::launch::async,[&]{return core.Handle(newer(acquire));});held.get_future().wait();
  auto reader=std::async(std::launch::async,[&]{return core.Snapshot();});
  assert(reader.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout);
  release.set_value();assert(writer.get().result==Result::Acquired);auto value=reader.get();
  assert(value.state==Readiness::RecoveryRequired&&value.fence_epoch==2);
  snapshot(core,store,h,Readiness::RecoveryRequired,2);
 }else if(mode=="wire"){
'''


class OutputFenceSnapshotTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        saved = base.PROGRAM
        program = saved.replace("int main(int argc", HELPERS + "\nint main(int argc")
        program = program.replace(' if(mode=="wire"){', CASES)
        program = program.replace('auto run=std::move(during_hold);run();',
                                  'auto run=std::move(during_hold);during_hold={};run();')
        try:
            base.PROGRAM = program
            base.OutputFenceTests.setUpClass.__func__(cls)
        finally:
            base.PROGRAM = saved

    run_case = base.OutputFenceTests.run_case

    def test_cold_missing_and_only_explicit_hydration_change_snapshot(self):
        self.run_case("snapshot_cold_missing")

    def test_invalid_identity_corrupt_or_faulted_storage_has_no_trusted_epoch(self):
        self.run_case("snapshot_invalid_storage")

    def test_every_durable_phase_after_reboot_uses_actual_core_result(self):
        self.run_case("snapshot_reboot_phases")

    def test_physical_failure_and_exact_retry_change_cached_state_only(self):
        self.run_case("snapshot_physical_failure")

    def test_unknown_commit_needs_explicit_rehydration_to_trust_epoch(self):
        self.run_case("snapshot_unknown_commit")

    def test_successful_lifecycle_and_stale_commands_preserve_readiness(self):
        self.run_case("snapshot_lifecycle")

    def test_concurrent_snapshot_serializes_with_actual_transition(self):
        self.run_case("snapshot_serialization")

    def test_abort_first_commit_unknown_closure_and_terminal_snapshot(self):
        self.run_case("snapshot_abort")


if __name__ == "__main__":
    unittest.main()
