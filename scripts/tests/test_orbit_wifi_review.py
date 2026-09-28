"""Regression tests for independently reviewed network boundaries and lifecycle locks."""
import unittest
from test_provisions_audio_boundaries import ROOT, method, run_cpp

class OrbitWifiReviewTests(unittest.TestCase):
    def test_actual_dns_reply_boundaries(self):
        program = r'''
#include <array>
#include <cassert>
#include <vector>
#include "__ROOT__/components/esp-wifi-connect/include/orbit_dns_reply.h"
int main(){
 const uint8_t ip[]={192,168,4,1};
 const std::vector<uint8_t> query={0x12,0x34,1,0,0,1,0,0,0,0,0,0,4,'b','o','a','t',0,0,1,0,1};
 // Every received length, including the formerly overflowing 497–512 range.
 for(size_t length=0;length<=1024;++length){
  std::array<uint8_t,1024> packet{};std::copy(query.begin(),query.end(),packet.begin());
  auto result=OrbitDnsReply(packet.data(),length,512,ip);
  if(length<query.size()||length>512)assert(result==0);
  else {assert(result==query.size()+16);assert(packet[0]==0x12&&packet[1]==0x34);assert(packet[7]==1);}
 }
 // Exact output capacity and guarded tail.
 for(size_t capacity=0;capacity<=64;++capacity){
  std::array<uint8_t,128> packet;packet.fill(0xa5);std::copy(query.begin(),query.end(),packet.begin());
  auto result=OrbitDnsReply(packet.data(),query.size(),capacity,ip);
  assert((result!=0)==(capacity>=query.size()+16));
  for(size_t i=std::max(capacity,query.size());i<packet.size();++i)assert(packet[i]==0xa5);
 }
 auto packet=query;packet.resize(512);
 packet[2]|=0x80;assert(OrbitDnsReply(packet.data(),query.size(),512,ip)==0);packet[2]=1;
 packet[12]=0xc0;assert(OrbitDnsReply(packet.data(),query.size(),512,ip)==0);packet[12]=64;
 assert(OrbitDnsReply(packet.data(),query.size(),512,ip)==0);
 packet.assign(query.begin(),query.end());packet.resize(512);packet[19]=28; // AAAA -> NODATA
 assert(OrbitDnsReply(packet.data(),query.size(),512,ip)==query.size());assert(packet[7]==0);
 // Deterministic hostile packet corpus under ASan/UBSan.
 uint32_t random=0xdeadbeef;
 for(int trial=0;trial<20000;++trial){std::array<uint8_t,513> bytes{};
  for(auto& byte:bytes){random=random*1664525+1013904223;byte=random>>24;}
  size_t length=trial%514;assert(OrbitDnsReply(bytes.data(),length,512,ip)<=512);
 }
}
'''
        run_cpp(program.replace('__ROOT__',str(ROOT)))

    def test_actual_manager_stop_allows_inflight_callbacks(self):
        path='components/esp-wifi-connect/wifi_manager.cc'
        methods='\n'.join(method(path,signature) for signature in (
            'void WifiManager::NotifyEvent(', 'void WifiManager::StartStation()',
            'void WifiManager::StopStation()', 'void WifiManager::StartConfigAp()',
            'void WifiManager::StopConfigAp()', 'bool WifiManager::IsConfigMode() const'))
        program=r'''
#include <cassert>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <future>
#include <chrono>
using namespace std::chrono_literals;
enum class WifiEvent{Scanning,Connecting,Connected,Disconnected,ConfigModeEnter,ConfigModeExit};
struct WifiStation{
 std::function<void()> scan,stop;std::function<void(int)> disconnected;
 void SetScanIntervalRange(int,int){}void SetFailureRetryCnt(int){}void SetHostname(const std::string&){}
 void OnScanBegin(std::function<void()> f){scan=f;}void OnConnect(std::function<void(const std::string&)>){}
 void OnConnected(std::function<void(const std::string&)>){}void OnDisconnected(std::function<void(int)> f){disconnected=f;}
 void Start(){if(scan)scan();}
 void Stop(){if(stop)stop();}
};
struct WifiConfigurationAp{
 std::function<void()> stop,exit;int starts=0;
 void SetSsidPrefix(const std::string&){}void SetLanguage(const std::string&){}
 void SetShowOtaConfig(bool){}void SetShowSleepConfig(bool){}
 void OnExitRequested(std::function<void()> f){exit=f;}void Start(){++starts;}void Stop(){if(stop)stop();}
};
struct WifiManager{
 mutable std::mutex mutex_;std::mutex lifecycle_mutex_;
 bool initialized_=true,station_active_=false,config_mode_active_=false;
 std::unique_ptr<WifiStation> station_=std::make_unique<WifiStation>();
 std::unique_ptr<WifiConfigurationAp> config_ap_=std::make_unique<WifiConfigurationAp>();
 struct Config{int station_scan_min_interval_seconds=1,station_scan_max_interval_seconds=5,station_failure_retry_cnt=1;
 std::string station_hostname,ssid_prefix,language;bool show_ota_config=false,show_sleep_config=false;}config_;
 std::function<void(WifiEvent,const std::string&)> event_callback_;
 void NotifyEvent(WifiEvent,const std::string& data="");void StartStation();void StopStation();void StartConfigAp();void StopConfigAp();
 bool IsConfigMode()const;
};
__METHODS__
int main(){
 WifiManager m;int scanned=0,exited=0;
 m.event_callback_=[&](WifiEvent event,const std::string&){
  (void)m.IsConfigMode(); // In-flight event callback queries state as the real board does.
  if(event==WifiEvent::Scanning)++scanned;
  if(event==WifiEvent::ConfigModeExit){++exited;m.StartStation();} // Real reconnect path.
 };
 m.StartStation();assert(scanned==1);
 m.station_->stop=[&](){std::thread event([&](){m.station_->disconnected(0);});event.join();};
 m.config_ap_->stop=[&](){std::thread event([&](){m.NotifyEvent(WifiEvent::Disconnected);});event.join();};
 for(int cycle=0;cycle<10;++cycle){m.StartConfigAp();assert(m.IsConfigMode());m.StopConfigAp();assert(!m.IsConfigMode());}
 assert(exited==10&&scanned==11);
 m.StopStation();assert(!m.station_active_);
 // A blocked stop serializes competing lifecycle operations while state queries stay live.
 m.StartStation();std::promise<void> entered,release;auto gate=release.get_future();
 m.station_->stop=[&](){entered.set_value();gate.wait();};
 auto first=std::async(std::launch::async,[&](){m.StartConfigAp();});entered.get_future().wait();
 auto second=std::async(std::launch::async,[&](){m.StartConfigAp();});
 assert(second.wait_for(20ms)==std::future_status::timeout);assert(!m.IsConfigMode());
 release.set_value();first.get();second.get();assert(m.config_ap_->starts==11);
}
'''
        run_cpp(program.replace('__METHODS__',methods))

    def test_actual_ap_owns_sta_and_drains_callbacks_before_timer_delete(self):
        path='components/esp-wifi-connect/wifi_configuration_ap.cc'
        methods='\n'.join(method(path,s) for s in ('void WifiConfigurationAp::Start()',
            'void WifiConfigurationAp::StartAccessPoint()', 'void WifiConfigurationAp::Stop()'))
        program=r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <memory>
#include <string>
#define CONFIG_PROVISIONS_GATEWAY_REQUIRED 1
#define CONFIG_IDF_TARGET_ESP32P4 1
#define ESP_LOGI(...) ((void)0)
#define ESP_ERROR_CHECK(x) assert((x)==0)
constexpr int ESP_OK=0,ESP_EVENT_ANY_ID=0,WIFI_EVENT=1,IP_EVENT=2,IP_EVENT_STA_GOT_IP=3,ESP_TIMER_TASK=0;
constexpr int WIFI_AUTH_OPEN=0,WIFI_AUTH_WPA2_PSK=2,WIFI_STORAGE_RAM=0,WIFI_MODE_APSTA=3,WIFI_IF_AP=1,WIFI_PS_NONE=0,WIFI_BAND_MODE_2G_ONLY=0,NVS_READONLY=0;
using esp_err_t=int;using nvs_handle_t=int;
struct esp_netif_t{int kind;};esp_netif_t* ap=nullptr;esp_netif_t* sta=nullptr;
struct Address{unsigned addr=0;};struct esp_netif_ip_info_t{Address ip,gw,netmask;};
#define IP4_ADDR(p,a,b,c,d) ((p)->addr=1)
esp_netif_t* esp_netif_create_default_wifi_ap(){assert(!ap);return ap=new esp_netif_t{1};}
esp_netif_t* esp_netif_create_default_wifi_sta(){assert(!sta);return sta=new esp_netif_t{2};}
void esp_netif_destroy_default_wifi(esp_netif_t* p){if(p==sta)sta=nullptr;else if(p==ap)ap=nullptr;else assert(false);delete p;}
int esp_netif_dhcps_stop(esp_netif_t*){return 0;}int esp_netif_dhcps_start(esp_netif_t*){return 0;}
int esp_netif_set_ip_info(esp_netif_t*,esp_netif_ip_info_t*){return 0;}
struct wifi_config_t{struct{unsigned char ssid[32],password[64];size_t ssid_len;int max_connection,authmode;}ap;};
int esp_wifi_set_storage(int){return 0;}int esp_wifi_set_mode(int){return 0;}int esp_wifi_set_ps(int){return 0;}
int esp_wifi_set_config(int,wifi_config_t* c){assert(c->ap.max_connection==1&&c->ap.authmode==WIFI_AUTH_WPA2_PSK);return 0;}
int esp_wifi_start(){assert(ap&&sta);return 0;}int esp_wifi_stop(){return 0;}int esp_wifi_set_band_mode(int){return 0;}
int esp_wifi_scan_start(void*,bool){return 0;}int esp_wifi_set_max_tx_power(int){return 0;}int esp_wifi_get_max_tx_power(int8_t*){return 0;}
int nvs_open(const char*,int,int*){return -1;}int nvs_get_str(int,const char*,char*,size_t*){return -1;}
int nvs_get_i8(int,const char*,int8_t*){return -1;}int nvs_get_u8(int,const char*,uint8_t*){return -1;}void nvs_close(int){}
size_t strlcpy(char* d,const char* s,size_t n){auto len=strlen(s);if(n){memcpy(d,s,std::min(len,n-1));d[std::min(len,n-1)]=0;}return len;}
struct esp_timer_create_args_t{void(*callback)(void*);void* arg;int dispatch_method;const char* name;bool skip_unhandled_events;};
bool timer_exists=false,registered=false,armed=false;int drained=0;
int esp_timer_create(esp_timer_create_args_t*,void** timer){assert(!timer_exists);timer_exists=true;*timer=reinterpret_cast<void*>(3);return 0;}
int esp_timer_stop(void*){armed=false;return 0;}
int esp_timer_delete(void*){assert(!registered&&!armed);timer_exists=false;return 0;}
int esp_event_handler_instance_register(int event,int,void(*)(void*,int,int,void*),void*,void** instance){assert(timer_exists);*instance=reinterpret_cast<void*>(static_cast<intptr_t>(event));if(event==WIFI_EVENT)registered=true;return 0;}
int esp_event_handler_instance_unregister(int event,int,void*){if(event==WIFI_EVENT){assert(timer_exists);armed=true;registered=false;++drained;}return 0;}
int httpd_stop(void*){return 0;}
struct DnsServer{void Start(Address){}void Stop(){}};
struct OrbitWifiSession{enum class Result{Active};};
struct WifiConfigurationAp{
 std::string orbit_password_=std::string(24,'a'),ota_url_;std::atomic<bool> is_connecting_{false};
 void* scan_timer_=nullptr;void* instance_any_id_=nullptr;void* instance_got_ip_=nullptr;void* server_=nullptr;
 esp_netif_t* ap_netif_=nullptr;esp_netif_t* setup_station_netif_=nullptr;
 std::unique_ptr<DnsServer> dns_server_;int8_t max_tx_power_=0;bool remember_bssid_=false,sleep_mode_=false;
 static void WifiEventHandler(void*,int,int,void*){}static void IpEventHandler(void*,int,int,void*){}
 OrbitWifiSession::Result OrbitSetupResult(){return OrbitWifiSession::Result::Active;}
 void CancelOrbitSetup(){}void StartWebServer(){assert(timer_exists);}
 std::string GetSsid(){return "Provisions-9594";}
 void Start();void StartAccessPoint();void Stop();
};
__METHODS__
int main(){WifiConfigurationAp setup;for(int cycle=0;cycle<20;++cycle){
 setup.orbit_password_=std::string(24,'a');setup.Start();assert(ap&&sta&&registered);
 setup.Stop();assert(!ap&&!sta&&!timer_exists&&!registered);assert(setup.orbit_password_.empty());
 }assert(drained==20);}
'''
        run_cpp(program.replace('__METHODS__',methods))
