#include <esp_timer.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <algorithm>
#include <cstring>
#include "sdkconfig.h"
#include "wifi_configuration_ap.h"

void WifiConfigurationAp::PrepareOrbitSetup(const std::string& password) {
    orbit_password_ = password;
    orbit_session_.Begin(esp_timer_get_time());
}
void WifiConfigurationAp::DiscardOrbitSetup() {
    // Called by the owner only after HTTP has stopped, including pre-start cancellation.
    orbit_session_.Cancel();
    std::fill(orbit_password_.begin(), orbit_password_.end(), '\0');
    orbit_password_.clear();
}
void WifiConfigurationAp::CancelOrbitSetup() {
    orbit_session_.Cancel();
    xEventGroupSetBits(event_group_, BIT1);  // Wake an in-flight connection test.
}
OrbitWifiSession::Result WifiConfigurationAp::OrbitSetupResult() {
    return orbit_session_.Poll(esp_timer_get_time());
}

#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
namespace {
constexpr char kPage[] =
    R"HTML(<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Orbit Wi-Fi</title>
<style>*{box-sizing:border-box}body{font:17px system-ui;background:#202326;color:#eee;max-width:28em;margin:0 auto;padding:2em 1.2em}h1{font-size:2em;margin:.3em 0}p{line-height:1.5}label{display:block;margin-top:1.2em}input,select,button{width:100%;padding:.85em;margin:.4em 0;font:inherit;border-radius:.65em}input,select{background:#303438;color:#fff;border:1px solid #777}button{background:#f6cd1c;color:#202326;border:0;font-weight:600;cursor:pointer}button:disabled{opacity:.55;cursor:wait}.secondary{background:transparent;color:#eee;border:1px solid #777}.hint,small{color:#ccc;font-size:.9em}details{margin:1.2em 0;line-height:1.5}summary{cursor:pointer}#status{padding:1em 0;white-space:pre-line}#status:empty{display:none}[hidden]{display:none!important}:focus-visible{outline:3px solid #f6cd1c;outline-offset:3px}</style>
<small>ORBIT · WI-FI SETUP</small><h1>Connect your Orbit</h1><p id="intro">Choose the Wi-Fi you want Orbit to use. Your phone is temporarily connected to Orbit for setup.</p>
<details><summary>Need to find your Wi-Fi password?</summary><p>Leave the QR screen open on Orbit. You can switch apps on your phone to copy the password, then return here and paste it.</p><p>If this window closes, stay connected to the Orbit hotspot and open <strong>http://192.168.4.1</strong> in your browser. Setup stays open for five minutes. If it has expired, open a new QR code on Orbit.</p></details>
<form id="f"><label for="net">Your Wi-Fi network</label><select id="net"><option value="">Looking for networks…</option></select><button type="button" class="secondary" id="scan">Refresh networks</button>
<label id="manual" hidden>Network name<input id="ssid" maxlength="32" autocomplete="off" autocapitalize="none" spellcheck="false"></label><small>Orbit uses 2.4 GHz Wi-Fi. For a hidden network, choose “Enter network name”.</small>
<label for="pass">Wi-Fi password</label><input id="pass" type="password" maxlength="64" autocomplete="off" autocapitalize="none" spellcheck="false"><button type="button" class="secondary" id="show" aria-pressed="false">Show password</button><small>Leave blank only if this network has no password.</small><button id="save">Connect Orbit</button></form><p id="status" role="status" aria-live="polite"></p><button class="secondary" id="cancel">Cancel setup</button>
<script>
const token='@TOKEN@';
const el=id=>document.getElementById(id);
const [f,net,ssid,pass,save,cancel,status,scanButton,show,manual]=['f','net','ssid','pass','save','cancel','status','scan','show','manual'].map(el);
let busy=false,scanning=false,finished=false;
const remember=value=>{try{if(value)sessionStorage.setItem('orbit-network',value);else sessionStorage.removeItem('orbit-network');}catch{}};
try{ssid.value=sessionStorage.getItem('orbit-network')||'';}catch{}
function selection(){const custom=net.selectedIndex===net.options.length-1;manual.hidden=!custom;ssid.value=custom?ssid.value:net.value;remember(ssid.value);if(custom)ssid.focus();}
net.onchange=selection;ssid.oninput=()=>remember(ssid.value);
show.onclick=()=>{const visible=pass.type==='password';pass.type=visible?'text':'password';show.textContent=visible?'Hide password':'Show password';show.setAttribute('aria-pressed',String(visible));};
async function scan(){if(scanning||busy||finished)return;scanning=true;scanButton.disabled=true;status.textContent='Looking for nearby Wi-Fi…';let names=[];try{for(let attempt=0;attempt<3;attempt++){try{const r=await fetch('/scan',{cache:'no-store'});if(!r.ok)throw Error();const a=await r.json();names=[...new Set(a.aps.map(n=>n.ssid).filter(Boolean))];if(names.length)break;}catch{}if(attempt<2)await new Promise(resolve=>setTimeout(resolve,1500));}if(busy||finished)return;net.replaceChildren(new Option('Choose your network',''));for(const name of names)net.add(new Option(name,name));net.add(new Option('Enter network name','__manual__'));if(names.includes(ssid.value)){net.value=ssid.value;manual.hidden=true;}else if(ssid.value){net.selectedIndex=net.options.length-1;manual.hidden=false;}else{net.value='';manual.hidden=names.length>0;}status.textContent=names.length?'':'Orbit could not list networks. Enter the network name or tap Refresh networks.';}finally{scanning=false;scanButton.disabled=busy||finished;}}
scanButton.onclick=scan;
function controls(disabled){for(const e of [net,ssid,pass,save,cancel,scanButton,show])e.disabled=disabled;save.textContent=disabled?'Checking Wi-Fi…':'Connect Orbit';}
f.onsubmit=async e=>{e.preventDefault();if(busy||finished)return;if(!ssid.value){manual.hidden=false;status.textContent='Choose your network or enter its name.';ssid.focus();return;}busy=true;controls(true);status.textContent='Checking the password and connection…\nKeep this page open. Orbit will return to its menu when setup finishes.';try{const r=await fetch('/submit',{method:'POST',headers:{'Content-Type':'application/json','X-Orbit-Setup':token},body:JSON.stringify({ssid:ssid.value,password:pass.value})});if(!r.ok)throw Error();const j=await r.json();if(j.success){finished=true;remember('');pass.value='';f.hidden=true;cancel.hidden=true;el('intro').textContent='Your Wi-Fi details are saved.';status.textContent='Orbit is reconnecting to your Wi-Fi.\nYou can close this page and return your phone to its usual Wi-Fi. Once Orbit is ready, try a voice request.';}else{status.textContent=j.error||'Could not connect. Check your password and try again.';}}catch{status.textContent='Orbit may have switched networks while testing your Wi-Fi. This does not mean the password was rejected.\nCheck Orbit: if it says “wi-fi saved — reconnecting”, setup worked. If its QR remains after 30 seconds, rejoin the Provisions hotspot and try Connect Orbit again.';}finally{busy=false;controls(false);}};
cancel.onclick=async()=>{if(busy||finished)return;busy=true;controls(true);try{const r=await fetch('/exit',{method:'POST',headers:{'X-Orbit-Setup':token}});if(!r.ok)throw Error();const j=await r.json();if(!j.success)throw Error();finished=true;remember('');pass.value='';f.hidden=true;cancel.hidden=true;status.textContent='Setup closed. Orbit is returning to its saved Wi-Fi.';}catch{status.textContent='Could not confirm cancellation. Press blue on Orbit to close setup.';}finally{busy=false;controls(false);}};
scan();
</script></html>)HTML";
bool IsLocalHost(httpd_req_t* req) {
    char host[40]{};
    return httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) == ESP_OK &&
           (strcmp(host, "192.168.4.1") == 0 || strcmp(host, "192.168.4.1:80") == 0);
}
esp_err_t Reply(httpd_req_t* req, bool success, const char* error) {
    cJSON* json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "success", success);
    if (error)
        cJSON_AddStringToObject(json, "error", error);
    char* body = cJSON_PrintUnformatted(json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    auto result = httpd_resp_sendstr(req, body ? body : "{\"success\":false}");
    cJSON_free(body);
    cJSON_Delete(json);
    return result;
}
}  // namespace
void WifiConfigurationAp::StartOrbitWebServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 16;
    config.recv_wait_timeout = 2;
    config.send_wait_timeout = 2;
    ESP_ERROR_CHECK(httpd_start(&server_, &config));
    const auto add = [this](const char* uri, httpd_method_t method,
                            esp_err_t (*handler)(httpd_req_t*)) {
        httpd_uri_t route{};
        route.uri = uri;
        route.method = method;
        route.handler = handler;
        route.user_ctx = this;
        ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &route));
    };
    add("/", HTTP_GET, [](httpd_req_t* req) -> esp_err_t {
        auto* self = static_cast<WifiConfigurationAp*>(req->user_ctx);
        if (!IsLocalHost(req)) {
            httpd_resp_set_status(req, "302 Found");
            httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
            return httpd_resp_sendstr(req, "");
        }
        if (self->OrbitSetupResult() != OrbitWifiSession::Result::Active)
            return Reply(req, false, "Setup expired. Open setup again on Orbit.");
        std::string page = kPage;
        page.replace(page.find("@TOKEN@"), 7, self->orbit_password_);
        httpd_resp_set_type(req, "text/html");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
        return httpd_resp_send(req, page.data(), page.size());
    });
    add("/scan", HTTP_GET, [](httpd_req_t* req) -> esp_err_t {
        auto* self = static_cast<WifiConfigurationAp*>(req->user_ctx);
        if (!IsLocalHost(req) || self->OrbitSetupResult() != OrbitWifiSession::Result::Active)
            return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Setup unavailable");
        cJSON* json = cJSON_CreateObject();
        cJSON* aps = cJSON_AddArrayToObject(json, "aps");
        for (const auto& ap : self->GetAccessPoints()) {
            char ssid[33]{};
            memcpy(ssid, ap.ssid, 32);
            auto* entry = cJSON_CreateObject();
            cJSON_AddStringToObject(entry, "ssid", ssid);
            cJSON_AddItemToArray(aps, entry);
        }
        char* body = cJSON_PrintUnformatted(json);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        auto result = httpd_resp_sendstr(req, body ? body : "{\"aps\":[]}");
        cJSON_free(body);
        cJSON_Delete(json);
        return result;
    });
    add("/submit", HTTP_POST, [](httpd_req_t* req) -> esp_err_t {
        auto* self = static_cast<WifiConfigurationAp*>(req->user_ctx);
        char token[64]{};
        if (!IsLocalHost(req) ||
            httpd_req_get_hdr_value_str(req, "X-Orbit-Setup", token, sizeof(token)) != ESP_OK ||
            self->orbit_password_ != token)
            return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Open setup on Orbit first");
        if (req->content_len == 0 || req->content_len > 1024)
            return Reply(req, false, "Invalid request size.");
        std::string body(req->content_len, '\0');
        size_t read = 0;
        while (read < body.size()) {
            if (self->OrbitSetupResult() != OrbitWifiSession::Result::Active)
                return Reply(req, false, "Setup expired.");
            int n = httpd_req_recv(req, body.data() + read, body.size() - read);
            if (n <= 0)
                return Reply(req, false, "Incomplete request. Try again.");
            read += n;
        }
        if (body.find("\\u0000") != std::string::npos || body.find('\0') != std::string::npos)
            return Reply(req, false, "Invalid credentials.");
        if (!OrbitWifiSession::FlatJson(body))
            return Reply(req, false, "Invalid request structure.");
        cJSON* json = cJSON_ParseWithLengthOpts(body.c_str(), body.size() + 1, nullptr, true);
        const auto* name = cJSON_GetObjectItemCaseSensitive(json, "ssid");
        const auto* pass = cJSON_GetObjectItemCaseSensitive(json, "password");
        if (!cJSON_IsString(name) || !cJSON_IsString(pass)) {
            cJSON_Delete(json);
            return Reply(req, false, "Enter the network name and password.");
        }
        std::string ssid = name->valuestring, password = pass->valuestring;
        cJSON_Delete(json);
        std::fill(body.begin(), body.end(), '\0');
        if (!OrbitWifiSession::ValidCredentials(ssid, password))
            return Reply(req, false, "Check the network name and password length.");
        if (self->OrbitSetupResult() != OrbitWifiSession::Result::Active ||
            !self->ConnectToWifi(ssid, password)) {
            std::fill(password.begin(), password.end(), '\0');
            return Reply(
                req, false,
                "Could not connect. Check the password and 2.4 GHz network, then try again.");
        }
        bool saved = self->orbit_session_.Commit(esp_timer_get_time(),
                                                 [&]() { return self->Save(ssid, password); });
        std::fill(password.begin(), password.end(), '\0');
        return Reply(
            req, saved,
            saved ? nullptr : "Could not save. Setup expired, was cancelled, or storage failed.");
    });
    add("/exit", HTTP_POST, [](httpd_req_t* req) -> esp_err_t {
        auto* self = static_cast<WifiConfigurationAp*>(req->user_ctx);
        char token[64]{};
        if (!IsLocalHost(req) ||
            httpd_req_get_hdr_value_str(req, "X-Orbit-Setup", token, sizeof(token)) != ESP_OK ||
            self->orbit_password_ != token)
            return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Open setup on Orbit first");
        self->CancelOrbitSetup();
        return Reply(req, true, nullptr);
    });
    for (const char* path :
         {"/hotspot-detect.html", "/generate_204", "/gen_204", "/mobile/status.php",
          "/check_network_status.txt", "/ncsi.txt", "/fwlink/", "/connectivity-check.html",
          "/success.txt", "/portal.html", "/library/test/success.html"}) {
        add(path, HTTP_GET, [](httpd_req_t* req) -> esp_err_t {
            httpd_resp_set_status(req, "302 Found");
            httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
            return httpd_resp_sendstr(req, "");
        });
    }
}
#else
void WifiConfigurationAp::StartOrbitWebServer() {}
#endif
