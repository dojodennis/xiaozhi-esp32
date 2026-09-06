"""Actual abort core/NVS CAS, origin-preserving v2 records, and v1 compatibility."""
import json
import unittest
from pathlib import Path

import test_provisions_output_fence as base

HELPERS = r'''
const std::string abort_message=R"wire(__ABORT__)wire";
const std::string abort_pending=R"wire(__ABORT_PENDING__)wire";
struct Authority:AbortAuthority {
 Message approved=parse(newer(abort_message));bool valid=true;unsigned calls=0;
 bool Allows(const Identity& id,const PhoneReceipt& receipt)override{++calls;return valid&&SameIdentity(id,approved.identity)&&SameReceipt(receipt,approved.receipt);}
};
std::vector<uint8_t> unhex(const std::string& hex){std::vector<uint8_t> bytes;for(size_t i=0;i<hex.size();i+=2)bytes.push_back(std::stoul(hex.substr(i,2),nullptr,16));return bytes;}
const char* legacy_hex[]={__LEGACY__};
std::string third(std::string text){text=newer(text);text=number(text,"fence_epoch",3);text=number(text,"sequence",3);text=change(text,"checkpoint_sha256",std::string(64,'c'));return change(text,"playback_id","eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee");}
bool json_equal(const std::string& a,const std::string& b){auto x=cJSON_Parse(a.c_str()),y=cJSON_Parse(b.c_str());assert(x&&y);bool same=cJSON_Compare(x,y,true);cJSON_Delete(x);cJSON_Delete(y);return same;}
'''
CASES = r'''
 if(mode=="abort_wire"){
  auto m=parse(abort_message);assert(m.command==Command::AbortUnacquired&&!m.receipt.completed&&!m.receipt.has_started);
  assert(json_equal(ReplyJson(Result::AbortPending,m.identity),abort_pending));
 }else if(mode=="abort_authority"){
  seed(Phase::Terminal);NvsStore s;Hardware h;Core raw(s,h,device);assert(raw.Hydrate());expect(raw,newer(abort_message),Result::Denied);assert(writes==0);
  Authority authority;authority.valid=false;Hardware hw;Core denied(s,hw,device,&authority);assert(denied.Hydrate());expect(denied,newer(abort_message),Result::Denied);assert(writes==0&&authority.calls==1);
 }else if(mode=="abort_receipt_freeze"){
  seed(Phase::Terminal);NvsStore s;Hardware h;Authority auth;Core c(s,h,device,&auth);assert(c.Hydrate());h.output=false;
  h.during_hold=[&]{auto r=read();assert(r.phase==Phase::AbortUnacquiredUnclosed&&r.origin==Origin::AbortUnacquired&&SameReceipt(r.receipt,parse(newer(abort_message)).receipt));};
  expect(c,newer(abort_message),Result::RecoveryRequired);assert(writes==1&&h.blocked&&!c.GateOpen());expect(c,newer(acquire),Result::Denied);expect(c,newer(close_commit),Result::Denied);expect(c,newer(release_message),Result::Denied);
  auto changed=number(newer(abort_message),"drained_at_ms",1981);auth.approved=parse(changed);expect(c,changed,Result::Denied);assert(writes==1);
  auth.approved=parse(newer(abort_message));h.output=true;auto pending=c.Handle(newer(abort_message));assert(pending.result==Result::AbortPending&&json_equal(pending.json,newer(abort_pending)));assert(writes==2&&h.blocked&&read().phase==Phase::AbortUnacquiredDrainedPendingCommit);
  auth.approved=parse(changed);expect(c,changed,Result::Denied);assert(writes==2);auth.approved=parse(newer(abort_message));expect(c,newer(abort_message),Result::AbortPending);assert(writes==2);
  expect(c,newer(close_commit),Result::Released);assert(writes==3&&c.GateOpen()&&read().origin==Origin::AbortUnacquired);expect(c,newer(abort_message),Result::Denied);expect(c,newer(acquire),Result::Denied);expect(c,newer(release_message),Result::Denied);expect(c,newer(close_commit),Result::Released);
  expect(c,third(acquire),Result::Acquired);expect(c,newer(close_commit),Result::Denied);expect(c,newer(abort_message),Result::Denied);assert(h.blocked&&read().identity.fence_epoch==3&&read().origin==Origin::Normal);
 }else if(mode=="abort_race"){
  for(bool abort_first:{false,true})for(bool drain_known:{false,true}){
   seed(Phase::Terminal);NvsStore s;Hardware h;Authority auth;Core c(s,h,device,&auth);assert(c.Hydrate());h.output=drain_known;std::thread later;Result second=Result::RecoveryRequired;
   h.during_hold=[&]{assert(read().identity.fence_epoch==2);later=std::thread([&]{second=c.Handle(abort_first?newer(acquire):newer(abort_message)).result;});};
   expect(c,abort_first?newer(abort_message):newer(acquire),drain_known?(abort_first?Result::AbortPending:Result::Acquired):Result::RecoveryRequired);later.join();assert(second==Result::Denied&&h.blocked&&!c.GateOpen());
   auto r=read();assert(r.origin==(abort_first?Origin::AbortUnacquired:Origin::Normal));assert(r.phase==(abort_first?(drain_known?Phase::AbortUnacquiredDrainedPendingCommit:Phase::AbortUnacquiredUnclosed):Phase::Owned));
   if(!abort_first){h.output=true;expect(c,newer(release_message),Result::DrainPending);expect(c,newer(close_commit),Result::Released);assert(read().origin==Origin::Normal);}
  }
 }else if(mode=="abort_faults"){
  for(int boundary:{1,2})for(auto f:{WriteOpen,Set,Commit,UncertainCommit,Readback}){
   seed(Phase::Terminal);NvsStore s;Hardware h;Authority auth;Core c(s,h,device,&auth);assert(c.Hydrate());fail_commit_at=boundary;targeted_fault=f;commit_attempts=0;
   expect(c,newer(abort_message),Result::RecoveryRequired);assert(h.blocked&&!c.GateOpen());expect(c,newer(abort_message),Result::RecoveryRequired);fault=None;targeted_fault=None;fail_commit_at=0;
   auto durable=read();Hardware reboot;Core next(s,reboot,device,&auth);assert(next.Hydrate());assert(next.GateOpen()==(durable.phase==Phase::Terminal));
   if(durable.phase!=Phase::Terminal){assert(durable.origin==Origin::AbortUnacquired&&SameReceipt(durable.receipt,parse(newer(abort_message)).receipt));expect(next,newer(acquire),Result::Denied);
    auto changed=number(newer(abort_message),"drained_at_ms",1981);auth.approved=parse(changed);expect(next,changed,Result::Denied);auth.approved=parse(newer(abort_message));}
   expect(next,newer(abort_message),Result::AbortPending);assert(!next.GateOpen()&&reboot.blocked);expect(next,newer(abort_message),Result::AbortPending);expect(next,newer(close_commit),Result::Released);assert(read().origin==Origin::AbortUnacquired);
  }
 }else if(mode=="abort_close_faults"){
  for(auto f:{WriteOpen,Set,Commit,UncertainCommit,Readback}){
   seed(Phase::Terminal);NvsStore s;Hardware h;Authority auth;Core c(s,h,device,&auth);assert(c.Hydrate());expect(c,newer(abort_message),Result::AbortPending);fail_commit_at=3;targeted_fault=f;expect(c,newer(close_commit),Result::RecoveryRequired);assert(!c.GateOpen()&&h.blocked);fault=None;targeted_fault=None;fail_commit_at=0;
   Hardware reboot;Core next(s,reboot,device,&auth);assert(next.Hydrate());assert(read().origin==Origin::AbortUnacquired);expect(next,newer(close_commit),Result::Released);expect(next,newer(abort_message),Result::Denied);expect(next,newer(close_commit),Result::Released);
  }
 }else if(mode=="abort_legacy"){
  for(int state=0;state<3;++state){committed=unhex(legacy_hex[state]);auto original=committed;fault=None;writes=0;NvsStore s;Hardware h;Authority auth;Core c(s,h,device,&auth);assert(c.Hydrate());assert(committed==original&&writes==0&&read().origin==Origin::Normal);
   if(state==0){expect(c,acquire,Result::Acquired);assert(committed==original);expect(c,release_message,Result::DrainPending);assert(committed[7]=='2'&&read().origin==Origin::Normal);}
   if(state==1){expect(c,release_message,Result::DrainPending);assert(committed==original);expect(c,close_commit,Result::Released);assert(committed[7]=='2'&&read().origin==Origin::Normal);}
   if(state==2){expect(c,close_commit,Result::Released);assert(committed==original);expect(c,newer(abort_message),Result::AbortPending);assert(committed[7]=='2'&&committed[11]==1&&read().origin==Origin::AbortUnacquired);}
  }
 }else if(mode=="abort_invalid_state"){
  for(auto phase:{Phase::Owned,Phase::DrainedPendingCommit}){seed(phase);NvsStore s;Hardware h;Authority auth;Core c(s,h,device,&auth);assert(c.Hydrate());expect(c,newer(abort_message),Result::Denied);assert(h.blocked&&writes==0);}
  committed.clear();fault=None;NvsStore s;Hardware h;Authority auth;Core missing(s,h,device,&auth);assert(!missing.Hydrate());expect(missing,newer(abort_message),Result::RecoveryRequired);assert(committed.empty()&&h.blocked);
  seed(Phase::Terminal);committed[9]^=1;Hardware corrupt;Core bad(s,corrupt,device,&auth);assert(!bad.Hydrate());expect(bad,newer(abort_message),Result::RecoveryRequired);assert(corrupt.blocked);
  seed(Phase::Terminal);Hardware hw;Core current(s,hw,device,&auth);assert(current.Hydrate());auto stale=number(newer(abort_message),"fence_epoch",1);auth.approved=parse(stale);expect(current,stale,Result::Denied);auto different=change(newer(abort_message),"device_id","eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee");auth.approved=parse(different);expect(current,different,Result::Denied);assert(writes==0);
 }else if(mode=="abort_store_transitions"){
  seed(Phase::Terminal);NvsStore s;auto old=read();auto unclosed=old;unclosed.identity=parse(newer(abort_message)).identity;unclosed.receipt=parse(newer(abort_message)).receipt;unclosed.origin=Origin::AbortUnacquired;unclosed.phase=Phase::AbortUnacquiredUnclosed;unclosed.backend_commit_id.clear();
  auto pending=unclosed;pending.phase=Phase::AbortUnacquiredDrainedPendingCommit;assert(!s.CompareExchange(old,pending));assert(s.CompareExchange(old,unclosed));
  auto changed=pending;++changed.receipt.drained_at_ms;assert(!s.CompareExchange(unclosed,changed));auto terminal=pending;terminal.phase=Phase::Terminal;terminal.backend_commit_id=uid_not_available;
 }else if(mode=="wire"){
'''
# The final store checks use the fixture backend UUID; avoid adding an unrelated generator.
CASES = CASES.replace('uid_not_available;', '''parse(close_commit).backend_commit_id;assert(!s.CompareExchange(unclosed,terminal));assert(s.CompareExchange(unclosed,pending));auto normal=pending;normal.origin=Origin::Normal;normal.phase=Phase::DrainedPendingCommit;assert(!s.CompareExchange(pending,normal));assert(s.CompareExchange(pending,terminal));assert(read().origin==Origin::AbortUnacquired);
  Manifest encoded;auto invalid=terminal;invalid.receipt.completed=true;assert(!EncodeRecord(invalid,encoded));invalid=terminal;invalid.phase=Phase::Owned;assert(!EncodeRecord(invalid,encoded));''')


class AbortOutputFenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        fixture = json.loads(base.FIXTURE.read_text())["messages"]
        abort = {**fixture["acquire"], "type": "output_fence_v1.abort_unacquired"}
        del abort["mode"]
        abort.update(completed=False, started_at_ms=None, stopped_at_ms=1480, drained_at_ms=1980)
        pending = {**fixture["drain_pending"], "type": "output_fence_v1.abort_pending", "acquire_rejected": True}
        legacy = json.loads((Path(__file__).parent / "fixtures/output_fence_nvs_v1.json").read_text())
        helpers = HELPERS.replace("__ABORT__", json.dumps(abort)).replace("__ABORT_PENDING__", json.dumps(pending))
        helpers = helpers.replace("__LEGACY__", ",".join(json.dumps(legacy["records"][key]) for key in ["owned", "pending", "terminal"]))
        saved = base.PROGRAM
        program = saved.replace("int main(int argc", helpers + "\nint main(int argc")
        program = program.replace(' if(mode=="wire"){', CASES)
        # A moved-from std::function may retain its callable; these barriers are one-shot.
        program = program.replace('auto run=std::move(during_hold);run();', 'auto run=std::move(during_hold);during_hold={};run();')
        program = program.replace('Fault fault=None;', 'Fault fault=None,targeted_fault=None;int fail_commit_at=0,commit_attempts=0;')
        program = program.replace('if((mode==NVS_READONLY&&fault==ReadOpen)', 'if(mode==NVS_READWRITE&&targeted_fault==WriteOpen&&commit_attempts+1==fail_commit_at)return 2;if((mode==NVS_READONLY&&fault==ReadOpen)')
        program = program.replace('if(fault==Set)return 2;', 'if(fault==Set||(targeted_fault==Set&&commit_attempts+1==fail_commit_at))return 2;')
        program = program.replace('int nvs_commit(nvs_handle_t){if(fault==Commit)return 2;committed=staged;did_write=true;++writes;return fault==UncertainCommit?2:0;}',
                                  'int nvs_commit(nvs_handle_t){++commit_attempts;auto active=commit_attempts==fail_commit_at?targeted_fault:fault;if(active==Commit)return 2;committed=staged;did_write=true;++writes;if(active==Readback)fault=Readback;return active==UncertainCommit?2:0;}')
        program = program.replace('fault=None;did_write=false;writes=0;opens=0;', 'fault=None;did_write=false;writes=0;opens=0;targeted_fault=None;fail_commit_at=0;commit_attempts=0;')
        try:
            base.PROGRAM = program
            base.OutputFenceTests.setUpClass.__func__(cls)
        finally:
            base.PROGRAM = saved
        cls.abort = abort

    run_case = base.OutputFenceTests.run_case

    def test_abort_wire_exact_reply(self): self.run_case("abort_wire")
    def test_abort_requires_registered_grant_and_receipt_authority(self): self.run_case("abort_authority")
    def test_first_cas_freezes_entire_receipt_before_drain_and_retries(self): self.run_case("abort_receipt_freeze")
    def test_acquire_abort_race_both_orders_with_unknown_drain(self): self.run_case("abort_race")
    def test_actual_nvs_faults_before_after_both_abort_commits(self): self.run_case("abort_faults")
    def test_abort_terminal_faults_reboot_and_ack_loss(self): self.run_case("abort_close_faults")
    def test_actual_v1_records_hydrate_without_automatic_migration(self): self.run_case("abort_legacy")
    def test_abort_never_initializes_or_replaces_unknown_owner(self): self.run_case("abort_invalid_state")
    def test_actual_nvs_forbids_phase_skip_origin_change_or_receipt_change(self): self.run_case("abort_store_transitions")

    def test_abort_wire_rejects_open_ambiguous_or_started_receipt(self):
        invalid = []
        for key in self.abort:
            changed = dict(self.abort); del changed[key]; invalid.append(changed)
        for patch in [{"extra": 1}, {"mode": "phone_exclusive"}, {"physical_state": "DRAINED_PENDING_COMMIT"},
                      {"completed": True}, {"completed": 0}, {"started_at_ms": 0}, {"stopped_at_ms": -1},
                      {"drained_at_ms": 1979}, {"device_connection_id": "00000000-0000-0000-0000-000000000000"}]:
            invalid.append({**self.abort, **patch})
        self.assertEqual(self.run_case("stdin", "\n".join(json.dumps(x) for x in invalid) + "\n").splitlines(), ["invalid"] * len(invalid))


if __name__ == "__main__":
    unittest.main()
