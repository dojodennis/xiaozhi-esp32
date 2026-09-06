"""Exercise the real Orbit transport public API with deterministic IDF I/O shims."""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

SHIM = r'''
#pragma once
#include <cstddef>
#include <cstdint>
struct FakeTransport;
using esp_transport_handle_t=FakeTransport*;
using TaskHandle_t=void*;
using esp_err_t=int;
constexpr int ESP_OK=0,pdPASS=1;
constexpr int ESP_TLS_ERR_SSL_WANT_READ=-101,ESP_TLS_ERR_SSL_WANT_WRITE=-102;
#define pdMS_TO_TICKS(ms) (ms)
using ConnectFn=int(*)(esp_transport_handle_t,const char*,int,int);
using ReadFn=int(*)(esp_transport_handle_t,char*,int,int);
using WriteFn=int(*)(esp_transport_handle_t,const char*,int,int);
using CloseFn=int(*)(esp_transport_handle_t);
using PollFn=int(*)(esp_transport_handle_t,int);
using DestroyFn=int(*)(esp_transport_handle_t);
using ws_transport_opcodes_t=int;
struct esp_transport_ws_config_t {const char* ws_path=nullptr;const char* headers=nullptr;bool propagate_control_frames=false;};
int64_t esp_timer_get_time();
int xTaskCreate(void(*)(void*),const char*,unsigned,void*,int,TaskHandle_t*);
void vTaskDelay(int);void vTaskDelete(void*);
int esp_crt_bundle_attach(void*);
esp_transport_handle_t esp_transport_ssl_init();
esp_transport_handle_t esp_transport_init();
esp_transport_handle_t esp_transport_ws_init(esp_transport_handle_t);
void esp_transport_ssl_crt_bundle_attach(esp_transport_handle_t,int(*)(void*));
void* esp_transport_get_context_data(esp_transport_handle_t);
void esp_transport_set_context_data(esp_transport_handle_t,void*);
void esp_transport_set_func(esp_transport_handle_t,ConnectFn,ReadFn,WriteFn,CloseFn,PollFn,PollFn,DestroyFn);
int esp_transport_connect_async(esp_transport_handle_t,const char*,int,int);
int esp_transport_connect(esp_transport_handle_t,const char*,int,int);
int esp_transport_read(esp_transport_handle_t,char*,int,int);
int esp_transport_write(esp_transport_handle_t,const char*,int,int);
int esp_transport_poll_read(esp_transport_handle_t,int);
int esp_transport_poll_write(esp_transport_handle_t,int);
int esp_transport_destroy(esp_transport_handle_t);
int esp_transport_ws_set_config(esp_transport_handle_t,const esp_transport_ws_config_t*);
int esp_transport_ws_get_upgrade_request_status(esp_transport_handle_t);
int esp_transport_ws_send_raw(esp_transport_handle_t,ws_transport_opcodes_t,const char*,int,int);
int esp_transport_ws_get_read_payload_len(esp_transport_handle_t);
ws_transport_opcodes_t esp_transport_ws_get_read_opcode(esp_transport_handle_t);
bool esp_transport_ws_get_fin_flag(esp_transport_handle_t);
'''

PROGRAM = r'''
#include "shim.h"
#include "provisions_websocket.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
using namespace std::chrono_literals;
namespace ProvisionsEndpointPolicy {const char* WebsocketUrl(){return "wss://app.provisions-app.com/approved";}}
const char* url=ProvisionsEndpointPolicy::WebsocketUrl();
struct Incoming {std::string payload;int opcode=1;bool final=true;size_t step=2048;size_t offset=0;};
struct FakeTransport {
    int kind=0;void* context=nullptr;FakeTransport* parent=nullptr;
    ConnectFn connect=nullptr;ReadFn read=nullptr;WriteFn write=nullptr;PollFn poll_read=nullptr,poll_write=nullptr;
    Incoming frame;bool active=false;
};
struct Fixture {
    std::atomic<int64_t> time{0};std::atomic<int> allocations{0},disposals{0},write_calls{0},max_poll{0};
    std::atomic<bool> writing{false},hold_poll{false},poll_held{false},connect_pending{false},want_write{false},want_read{false},poll_write_timeout{false};
    std::atomic<bool> callback_entered{false},callback_release{false};
    int allocation_failure=0;bool task_failure=false;int write_error_after=0,short_write=0,upgrade_status=101;
    int handshake_read_step=0;std::atomic<int> handshake_left{0};
    std::mutex mutex;std::condition_variable changed;std::deque<Incoming> frames;
    std::vector<std::thread> tasks;std::thread::id caller=std::this_thread::get_id(),worker;
    std::vector<std::pair<int,std::string>> sent;std::vector<std::pair<bool,std::string>> received;
    std::string wire;std::atomic<int> disconnected{0};
    ~Fixture(){for(auto& t:tasks)if(t.joinable())t.join();}
    template<typename F> void wait(F condition){
        auto end=std::chrono::steady_clock::now()+2s;
        while(!condition() && std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(1ms);
        assert(condition());
    }
    void inject(std::string payload,int opcode=1,bool final=true,size_t step=2048){std::lock_guard<std::mutex> lock(mutex);frames.push_back({payload,opcode,final,step});}
    size_t received_count(){std::lock_guard<std::mutex> lock(mutex);return received.size();}
    void callbacks(ProvisionsWebSocket& socket){
        socket.OnData([this](const char* data,size_t size,bool binary){std::lock_guard<std::mutex> lock(mutex);received.emplace_back(binary,std::string(data,size));});
        socket.OnDisconnected([this]{++disconnected;});
    }
    void closed(){wait([&]{return disposals.load()==allocations.load();});for(auto& t:tasks)if(t.joinable())t.join();}
};
Fixture* f;
thread_local FakeTransport* reading_ws=nullptr;
int64_t esp_timer_get_time(){return f->time.load();}
void vTaskDelay(int ticks){f->time.fetch_add(static_cast<int64_t>(ticks)*1000);std::this_thread::sleep_for(50us);}
void vTaskDelete(void*){}
int xTaskCreate(void(*fn)(void*),const char*,unsigned,void* arg,int,TaskHandle_t*){
    if(f->task_failure)return 0;
    f->tasks.emplace_back([fn,arg]{f->worker=std::this_thread::get_id();fn(arg);});return pdPASS;
}
int esp_crt_bundle_attach(void*){return 0;}
FakeTransport* allocate(int kind){int count=++f->allocations;if(count==f->allocation_failure){--f->allocations;return nullptr;}auto* t=new FakeTransport;t->kind=kind;return t;}
esp_transport_handle_t esp_transport_ssl_init(){return allocate(1);}
esp_transport_handle_t esp_transport_init(){return allocate(2);}
esp_transport_handle_t esp_transport_ws_init(esp_transport_handle_t parent){auto* t=allocate(3);if(t)t->parent=parent;return t;}
void esp_transport_ssl_crt_bundle_attach(esp_transport_handle_t,int(*)(void*)){}
void* esp_transport_get_context_data(esp_transport_handle_t t){return t->context;}
void esp_transport_set_context_data(esp_transport_handle_t t,void* data){t->context=data;}
void esp_transport_set_func(esp_transport_handle_t t,ConnectFn c,ReadFn r,WriteFn w,CloseFn,PollFn pr,PollFn pw,DestroyFn){t->connect=c;t->read=r;t->write=w;t->poll_read=pr;t->poll_write=pw;}
int esp_transport_connect_async(esp_transport_handle_t,const char*,int,int){return f->connect_pending?0:1;}
int esp_transport_ws_set_config(esp_transport_handle_t,const esp_transport_ws_config_t* config){assert(config->propagate_control_frames);return ESP_OK;}
int esp_transport_ws_get_upgrade_request_status(esp_transport_handle_t){return f->upgrade_status;}
int esp_transport_connect(esp_transport_handle_t t,const char* host,int port,int timeout){
    if(t->kind==2)return t->connect(t,host,port,timeout);
    assert(t->kind==3);if(esp_transport_connect(t->parent,host,port,timeout)<0)return -1;
    if(f->handshake_read_step){
        f->handshake_left=100;char byte;
        while(f->handshake_left>0){if(esp_transport_read(t->parent,&byte,1,timeout)<=0)return -1;}
    }
    return 0;
}
int esp_transport_read(esp_transport_handle_t t,char* data,int size,int timeout){
    if(t->kind==2)return t->read(t,data,size,timeout);
    if(t->kind==1){
        if(f->want_read)return ESP_TLS_ERR_SSL_WANT_READ;
        if(f->handshake_left>0){f->time.fetch_add(f->handshake_read_step);--f->handshake_left;*data='h';return 1;}
        assert(reading_ws);auto& frame=reading_ws->frame;
        const auto count=std::min<size_t>({static_cast<size_t>(size),frame.step,frame.payload.size()-frame.offset});
        std::memcpy(data,frame.payload.data()+frame.offset,count);frame.offset+=count;return count;
    }
    assert(t->kind==3);
    if(!t->active){std::lock_guard<std::mutex> lock(f->mutex);if(f->frames.empty())return 0;t->frame=std::move(f->frames.front());f->frames.pop_front();t->active=true;}
    reading_ws=t;
    int result=0;
    // IDF6.0.2 ws_read explicitly reads an entire propagated control payload
    // through read_exact_size; data frames retain ordinary short-read behavior.
    if(t->frame.opcode>=8){
        while(t->frame.offset<t->frame.payload.size()){
            const int count=esp_transport_read(t->parent,data+result,t->frame.payload.size()-t->frame.offset,timeout);
            if(count<=0){result=-1;break;}result+=count;
        }
    }else if(!t->frame.payload.empty())result=esp_transport_read(t->parent,data,std::min<int>(size,t->frame.payload.size()-t->frame.offset),timeout);
    reading_ws=nullptr;if(t->frame.offset==t->frame.payload.size())t->active=false;return result;
}
int esp_transport_write(esp_transport_handle_t t,const char* data,int size,int timeout){
    if(t->kind==2)return t->write(t,data,size,timeout);
    assert(t->kind==1);f->writing=true;++f->write_calls;
    if(f->want_write)return ESP_TLS_ERR_SSL_WANT_WRITE;
    if(f->write_error_after && f->write_calls>f->write_error_after)return -55;
    const int count=f->short_write?std::min(f->short_write,size):size;
    {std::lock_guard<std::mutex> lock(f->mutex);f->wire.append(data,count);}return count;
}
int esp_transport_poll_read(esp_transport_handle_t t,int timeout){
    if(t->kind==2)return t->poll_read(t,timeout);
    if(t->kind==3){if(t->active)return 1;{std::lock_guard<std::mutex> lock(f->mutex);if(!f->frames.empty())return 1;}return esp_transport_poll_read(t->parent,timeout);}
    f->max_poll.store(std::max(f->max_poll.load(),timeout));
    if(f->hold_poll){f->poll_held=true;while(f->hold_poll)std::this_thread::sleep_for(100us);}
    f->time.fetch_add(static_cast<int64_t>(timeout)*1000);std::this_thread::sleep_for(100us);return 0;
}
int esp_transport_poll_write(esp_transport_handle_t t,int timeout){
    if(t->kind==2)return t->poll_write(t,timeout);
    f->max_poll.store(std::max(f->max_poll.load(),timeout));
    if(f->poll_write_timeout){f->time.fetch_add(static_cast<int64_t>(timeout)*1000);return 0;}return 1;
}
int esp_transport_ws_send_raw(esp_transport_handle_t t,ws_transport_opcodes_t opcode,const char* data,int size,int timeout){
    if(esp_transport_poll_write(t->parent,timeout)<=0)return -1;
    const char header='H';if(esp_transport_write(t->parent,&header,1,timeout)!=1)return -1;
    const int written=size?esp_transport_write(t->parent,data,size,timeout):0;
    if(written==size){std::lock_guard<std::mutex> lock(f->mutex);f->sent.emplace_back(opcode,std::string(data,size));}return written;
}
int esp_transport_ws_get_read_payload_len(esp_transport_handle_t t){return t->frame.payload.size();}
ws_transport_opcodes_t esp_transport_ws_get_read_opcode(esp_transport_handle_t t){return t->frame.opcode;}
bool esp_transport_ws_get_fin_flag(esp_transport_handle_t t){return t->frame.final;}
int esp_transport_destroy(esp_transport_handle_t t){assert(std::this_thread::get_id()==f->worker);delete t;++f->disposals;return 0;}
void require_fast(std::function<void()> action){auto start=std::chrono::steady_clock::now();action();assert(std::chrono::steady_clock::now()-start<100ms);}
int main(int argc,char** argv){assert(argc==2);Fixture fixture;f=&fixture;const std::string test=argv[1];
    if(test=="short_writes"){
        ProvisionsWebSocket socket;f->short_write=2;assert(socket.Connect(url));assert(socket.Send("abcdefg"));
        {std::lock_guard<std::mutex> lock(f->mutex);assert(f->wire=="Habcdefg");assert(f->sent.size()==1 && f->sent[0].second=="abcdefg");}
        assert(f->write_calls==5);socket.Close();f->closed();
    }else if(test=="write_deadline" || test=="poll_deadline"){
        ProvisionsWebSocket socket;assert(socket.Connect(url));const auto start=esp_timer_get_time();
        f->want_write=test=="write_deadline";f->poll_write_timeout=test=="poll_deadline";
        assert(!socket.Send("blocked"));f->closed();assert(!socket.IsConnected());
        assert(esp_timer_get_time()-start>=3000000 && esp_timer_get_time()-start<=3040000);assert(f->max_poll<=20);
    }else if(test=="partial_error"){
        ProvisionsWebSocket socket;f->short_write=2;f->write_error_after=2;assert(socket.Connect(url));assert(!socket.Send("abcdefg"));f->closed();
        std::lock_guard<std::mutex> lock(f->mutex);assert(f->wire=="Hab" && f->sent.empty());
    }else if(test=="close_lifetime"){
        auto socket=std::make_unique<ProvisionsWebSocket>();assert(socket->Connect(url));f->want_write=true;
        assert(socket->SendAsync("queued"));f->wait([&]{return f->writing.load();});
        require_fast([&]{socket->Close();socket.reset();});f->closed();assert(f->max_poll<=20);
    }else if(test=="close_sync_wait"){
        ProvisionsWebSocket socket;assert(socket.Connect(url));f->want_write=true;std::atomic<bool> result{true};
        std::thread sender([&]{result=socket.Send("queued");});f->wait([&]{return f->writing.load();});
        require_fast([&]{socket.Close();});sender.join();assert(!result);f->closed();
    }else if(test=="destroy_from_callback"){
        auto socket=std::make_unique<ProvisionsWebSocket>();
        socket->OnData([&](const char*,size_t,bool){socket.reset();f->callback_entered=true;});
        socket->OnDisconnected([&]{++f->disconnected;});assert(socket->Connect(url));f->inject("delete");
        f->wait([&]{return f->callback_entered.load();});f->closed();assert(!socket && f->disconnected==1);
    }else if(test=="read_deadline"){
        ProvisionsWebSocket socket;f->callbacks(socket);assert(socket.Connect(url));f->want_read=true;
        const auto start=esp_timer_get_time();f->inject("stalled");f->wait([&]{return !socket.IsConnected();});f->closed();
        assert(f->received_count()==0);assert(esp_timer_get_time()-start>=500000 && esp_timer_get_time()-start<=560000);
    }else if(test=="queue_bounds"){
        ProvisionsWebSocket socket;f->hold_poll=true;assert(socket.Connect(url));f->wait([&]{return f->poll_held.load();});
        for(int i=0;i<4;++i)require_fast([&]{assert(socket.SendAsync("queued"));});
        require_fast([&]{assert(!socket.SendAsync("excess"));});socket.Close();f->hold_poll=false;f->closed();
        std::lock_guard<std::mutex> lock(f->mutex);assert(f->sent.empty());
    }else if(test=="connect_deadline" || test=="upgrade_deadline"){
        ProvisionsWebSocket socket;f->connect_pending=test=="connect_deadline";f->handshake_read_step=test=="upgrade_deadline"?100000:0;
        assert(!socket.Connect(url));f->closed();assert(esp_timer_get_time()>=8000000 && esp_timer_get_time()<=8100000);
    }else if(test=="data_fragments"){
        ProvisionsWebSocket socket;f->callbacks(socket);assert(socket.Connect(url));
        f->inject(std::string(3000,'a'),1,false,137);f->inject("p",9);f->inject(std::string(5000,'b'),0,true,197);
        f->wait([&]{return f->received_count()==1 || !socket.IsConnected();});assert(socket.IsConnected());
        {std::lock_guard<std::mutex> lock(f->mutex);assert(f->received.size()==1 && !f->received[0].first && f->received[0].second==std::string(3000,'a')+std::string(5000,'b'));assert(f->sent.size()==1 && f->sent[0].first==138 && f->sent[0].second=="p");}
        socket.Close();f->closed();
    }else if(test=="split_ping"){
        ProvisionsWebSocket socket;f->callbacks(socket);assert(socket.Connect(url));f->inject("ping",9,true,2);f->inject("after");
        f->wait([&]{return f->received_count()==1 || !socket.IsConnected();});assert(socket.IsConnected());
        {std::lock_guard<std::mutex> lock(f->mutex);assert(f->received.size()==1 && f->received[0].second=="after");assert(f->sent.size()==1 && f->sent[0].second=="ping");}
        socket.Close();f->closed();
    }else if(test=="binary_limit"){
        ProvisionsWebSocket socket;f->callbacks(socket);assert(socket.Connect(url));f->inject(std::string(2049,'b'),2);
        f->wait([&]{return !socket.IsConnected();});f->closed();assert(f->received_count()==0);
    }else if(test=="fragment_deadline"){
        ProvisionsWebSocket socket;f->callbacks(socket);assert(socket.Connect(url));f->inject("incomplete",1,false);
        const auto start=esp_timer_get_time();f->wait([&]{return !socket.IsConnected();});f->closed();assert(f->received_count()==0);assert(esp_timer_get_time()-start<=5560000);
    }else if(test=="invalid_inputs"){
        ProvisionsWebSocket invalid;assert(!invalid.Connect("wss://other.invalid/"));invalid.SetHeader("bad\r\nkey","secret");assert(!invalid.Connect(url));
        ProvisionsWebSocket socket;assert(socket.Connect(url));assert(!socket.Send(""));assert(!socket.Send(std::string(8193,'x')));assert(!socket.Send(nullptr,2,true));assert(!socket.Send(std::string(2049,'x').data(),2049,true));socket.Close();f->closed();
    }else if(test=="allocation_failures"){
        for(int fail=1;fail<=3;++fail){Fixture local;f=&local;f->allocation_failure=fail;ProvisionsWebSocket socket;assert(!socket.Connect(url));f->closed();}
        f=&fixture;f->task_failure=true;ProvisionsWebSocket socket;assert(!socket.Connect(url));
    }else assert(false);
    std::cout<<test<<" passed\n";
}
'''


class ProvisionsWebsocketReview(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="orbit-websocket-review-")
        cls.path = Path(cls.directory.name)
        (cls.path / "shim.h").write_text(SHIM)
        for name in ("esp_crt_bundle.h", "esp_timer.h", "esp_transport.h", "esp_transport_ssl.h", "esp_transport_ws.h", "freertos/FreeRTOS.h", "freertos/task.h"):
            target = cls.path / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text('#include "shim.h"\n')
        (cls.path / "review.cc").write_text(PROGRAM)
        cls.binary = cls.path / "review"
        built = subprocess.run([shutil.which("c++"), "-std=c++17", "-pthread", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                                "-I", str(cls.path), "-I", str(ROOT / "main/protocols"), "-I", str(ROOT / "main"),
                                str(cls.path / "review.cc"), str(ROOT / "main/protocols/provisions_websocket.cc"), "-o", str(cls.binary)], capture_output=True, text=True)
        if built.returncode:
            raise RuntimeError(built.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def run_case(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=10,
                                env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0" if sys.platform == "darwin" else "detect_leaks=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_partial_writes_and_fatal_error(self):
        for case in ("short_writes", "partial_error"):
            with self.subTest(case=case):
                self.run_case(case)

    def test_write_and_connect_deadlines(self):
        for case in ("write_deadline", "poll_deadline", "connect_deadline", "upgrade_deadline"):
            with self.subTest(case=case):
                self.run_case(case)

    def test_nonblocking_close_lifetime_and_queue_bounds(self):
        for case in ("close_lifetime", "close_sync_wait", "destroy_from_callback", "queue_bounds"):
            with self.subTest(case=case):
                self.run_case(case)

    def test_fragmented_data_and_limits(self):
        for case in ("data_fragments", "binary_limit", "fragment_deadline", "read_deadline"):
            with self.subTest(case=case):
                self.run_case(case)

    def test_control_payload_may_arrive_in_short_tls_reads(self):
        self.run_case("split_ping")

    def test_rejected_inputs_and_allocation_failures(self):
        for case in ("invalid_inputs", "allocation_failures"):
            with self.subTest(case=case):
                self.run_case(case)


if __name__ == "__main__":
    unittest.main()
