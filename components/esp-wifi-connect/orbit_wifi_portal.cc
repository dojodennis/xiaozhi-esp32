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
<style>body{font:18px system-ui;background:#202326;color:#eee;max-width:28em;margin:3em auto;padding:1em}input,select,button{box-sizing:border-box;width:100%;padding:.8em;margin:.5em 0;font:inherit}button{background:#f6cd1c;border:0;border-radius:.6em}small{color:#ccc}</style>
<h1>orbit wi-fi</h1><p>Choose a 2.4 GHz network.</p><form id="f"><label>Nearby networks<select id="net"><option value="">Choose a network</option></select></label><button type="button" id="scan">Refresh networks</button><label>Network name<input id="ssid" required maxlength="32" autocomplete="off"></label><label>Password<input id="pass" type="password" maxlength="64" autocomplete="off"></label><small>Leave blank only for an open network.</small><button id="save">Connect</button></form><p id="status" role="status"></p><button id="cancel">Cancel setup</button>
<script>const token='@TOKEN@';const status=document.getElementById('status');const [f,net,ssid,pass,save,cancel]=['f','net','ssid','pass','save','cancel'].map(id=>document.getElementById(id));let busy=false;
async function scan(){try{const r=await fetch('/scan');if(!r.ok)throw Error();const a=await r.json();net.replaceChildren(new Option('Choose a network',''));for(const n of a.aps)net.add(new Option(n.ssid,n.ssid));}catch{status.textContent='Cannot refresh. Check that your phone is connected to the Orbit hotspot.';}}
net.onchange=()=>ssid.value=net.value;document.getElementById('scan').onclick=scan;
f.onsubmit=async e=>{e.preventDefault();if(busy)return;busy=true;save.disabled=true;status.textContent='Checking Wi-Fi. Stay connected to Orbit…';try{const r=await fetch('/submit',{method:'POST',headers:{'Content-Type':'application/json','X-Orbit-Setup':token},body:JSON.stringify({ssid:ssid.value,password:pass.value})});const j=await r.json();status.textContent=j.success?'Wi-Fi saved. Orbit is reconnecting; your phone can leave this hotspot.':j.error;if(j.success){pass.value='';f.hidden=true;cancel.hidden=true;}}catch{status.textContent='Connection changed. Check the ring. If setup is still shown, reconnect your phone to Orbit and try again.';}finally{busy=false;save.disabled=false;}};
cancel.onclick=async()=>{await fetch('/exit',{method:'POST',headers:{'X-Orbit-Setup':token}}).catch(()=>{});status.textContent='Setup closed. Orbit will retry its saved networks.';f.hidden=true;cancel.hidden=true;};scan();</script></html>)HTML";
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
