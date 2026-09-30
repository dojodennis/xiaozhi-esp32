"""Exercise actual persistence and scan selection with preferred/fallback networks."""
import unittest
from test_provisions_audio_boundaries import method, run_cpp


class OrbitWifiPreferenceTests(unittest.TestCase):
    def test_selection_persists_and_failed_save_restores_order(self):
        methods = '\n'.join(method('components/esp-wifi-connect/ssid_manager.cc', signature)
                            for signature in ('void SsidManager::LoadFromNvs(',
                                              'bool SsidManager::SaveToNvs(',
                                              'bool SsidManager::AddSsid('))
        program = r'''
#include <algorithm>
#include <cassert>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#define CONFIG_PROVISIONS_GATEWAY_REQUIRED __CONFIG__
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define NVS_NAMESPACE "wifi"
#define MAX_WIFI_SSID_COUNT 10
constexpr int ESP_OK=0,NVS_READONLY=0,NVS_READWRITE=1;
using nvs_handle_t=int;
std::map<std::string,std::string> disk,pending;
bool fail_commit=false;
int nvs_open(const char*,int mode,int* handle){*handle=mode;pending=disk;return 0;}
int nvs_get_str(int,const char* key,char* out,size_t* length){
 auto found=disk.find(key);if(found==disk.end())return 1;
 assert(*length>found->second.size());strcpy(out,found->second.c_str());return 0;
}
int nvs_set_str(int,const char* key,const char* value){pending[key]=value;return 0;}
int nvs_erase_key(int,const char* key){pending.erase(key);return 0;}
int nvs_commit(int){if(fail_commit)return 1;disk=pending;return 0;}
void nvs_close(int){}
struct SsidItem{std::string ssid,password;};
struct SsidManager{
 std::vector<SsidItem> ssid_list_;
 void LoadFromNvs();bool SaveToNvs();bool AddSsid(const std::string&,const std::string&);
};
__METHODS__
int main(){
 SsidManager m;assert(m.AddSsid("old","old-secret"));
 assert(m.AddSsid("selected","selected-secret"));
 assert(m.ssid_list_.front().ssid=="selected");
 SsidManager reboot;reboot.LoadFromNvs();
 assert(reboot.ssid_list_.size()==2&&reboot.ssid_list_.front().ssid=="selected");
 assert(reboot.AddSsid("old","replacement-secret"));
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
 assert(reboot.ssid_list_.front().ssid=="old");
 assert(reboot.ssid_list_.front().password=="replacement-secret");
#else
 assert(reboot.ssid_list_.front().ssid=="selected");
#endif
 SsidManager again;again.LoadFromNvs();
 assert(again.ssid_list_.front().ssid==reboot.ssid_list_.front().ssid);
 auto previous=again.ssid_list_;auto saved=disk;
 fail_commit=true;assert(!again.AddSsid("selected","incorrect-unsaved-secret"));
 assert(disk==saved&&again.ssid_list_.size()==previous.size());
 for(size_t i=0;i<previous.size();++i){
  assert(again.ssid_list_[i].ssid==previous[i].ssid);
  assert(again.ssid_list_[i].password==previous[i].password);
 }
 assert(!again.AddSsid("new-unsaved","another-secret"));assert(disk==saved);
 assert(again.ssid_list_.size()==previous.size());
}
'''
        for config in (0, 1):
            with self.subTest(provisions=config):
                run_cpp(program.replace('__METHODS__', methods).replace('__CONFIG__', str(config)))

    def test_actual_scan_prefers_selected_then_falls_back_by_signal(self):
        scan = method('components/esp-wifi-connect/wifi_station.cc',
                      'void WifiStation::HandleScanResult(')
        program = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#define CONFIG_PROVISIONS_GATEWAY_REQUIRED __CONFIG__
#define ESP_LOGI(...) ((void)0)
struct wifi_ap_record_t {uint8_t ssid[33]{};uint8_t bssid[6]{};int rssi=0,primary=1,authmode=2;};
std::vector<wifi_ap_record_t> air;
void esp_wifi_scan_get_ap_num(uint16_t* n){*n=air.size();}
void esp_wifi_scan_get_ap_records(uint16_t*,wifi_ap_record_t* records){std::copy(air.begin(),air.end(),records);}
void esp_timer_start_once(int,int){}
struct SsidItem{std::string ssid,password;};
struct SsidManager{
 std::vector<SsidItem> profiles{{"selected","new-secret"},{"old","old-secret"},{"backup","backup-secret"}};
 static SsidManager& GetInstance(){static SsidManager value;return value;}
 std::vector<SsidItem> GetSsidList(){return profiles;}
};
struct WifiApRecord {std::string ssid,password;int channel,authmode;uint8_t bssid[6];};
struct WifiStation{
 std::vector<WifiApRecord> connect_queue_;
 int scan_current_interval_microseconds_=1000,timer_handle_=0,starts=0,empty_scans=0;
 void HandleScanResult();void StartConnect(){++starts;}
 void UpdateScanInterval(){++empty_scans;}
};
void add(const char* ssid,int signal,int bssid){wifi_ap_record_t ap;strcpy((char*)ap.ssid,ssid);ap.rssi=signal;ap.bssid[5]=bssid;air.push_back(ap);}
__SCAN__
int main(){
 add("old",-20,1);add("selected",-75,2);add("backup",-35,3);add("selected",-60,4);add("unknown",-10,5);
 WifiStation s;s.HandleScanResult();assert(s.starts==1&&s.connect_queue_.size()==4);
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
 assert(s.connect_queue_[0].ssid=="selected"&&s.connect_queue_[0].bssid[5]==4);
 assert(s.connect_queue_[1].ssid=="selected"&&s.connect_queue_[1].bssid[5]==2);
 assert(s.connect_queue_[2].ssid=="old"&&s.connect_queue_[3].ssid=="backup");
 // Removing exhausted preferred attempts leaves usable fallback candidates.
 s.connect_queue_.erase(s.connect_queue_.begin(),s.connect_queue_.begin()+2);
 assert(s.connect_queue_.front().ssid=="old");
#else
 assert(s.connect_queue_[0].ssid=="old"&&s.connect_queue_[1].ssid=="backup");
#endif
 air.clear();add("backup",-50,1);add("old",-25,2);
 WifiStation absent;absent.HandleScanResult();
 assert(absent.connect_queue_.size()==2&&absent.connect_queue_[0].ssid=="old");
 air.clear();WifiStation empty;empty.HandleScanResult();assert(empty.starts==0&&empty.empty_scans==1);
 SsidManager::GetInstance().profiles.clear();add("unknown",-20,3);
 WifiStation unconfigured;unconfigured.HandleScanResult();assert(unconfigured.starts==0);
}
'''
        for config in (0, 1):
            with self.subTest(provisions=config):
                run_cpp(program.replace('__SCAN__', scan).replace('__CONFIG__', str(config)))
