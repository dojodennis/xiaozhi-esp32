#include <cJSON.h>
#include "esp_timer.h"
#include "wifi_configuration_ap.h"
int main() {
    WifiConfigurationAp ap;
    const std::string secret = "abcdef012345abcdef012345";
    auto begin = [&]() {
        now_us = 0;
        ap.during_connect = {};
        ap.connect_ok = true;
        ap.save_ok = true;
        ap.PrepareOrbitSetup(secret);
        ap.StartOrbitWebServer();
    };
    auto request = [&](const char* path, const std::string& body = "",
                       const std::string& token = "abcdef012345abcdef012345",
                       const std::string& host = "192.168.4.1") {
        auto route = routes.at(path);
        httpd_req_t req;
        req.user_ctx = route.user_ctx;
        req.body = body;
        req.content_len = body.size();
        req.headers = {{"Host", host}, {"X-Orbit-Setup", token}};
        route.handler(&req);
        return req;
    };
    auto succeeded = [](const httpd_req_t& req) {
        auto* json = cJSON_Parse(req.response.c_str());
        bool ok = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "success"));
        cJSON_Delete(json);
        return ok;
    };
    begin();
    for (const char* removed : {"/advanced/config", "/advanced/submit", "/saved/list",
                                "/saved/delete", "/saved/set_default"})
        assert(!routes.count(removed));
    auto scan = request("/scan");
    auto* json = cJSON_Parse(scan.response.c_str());
    assert(json);
    assert(std::string(
               cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(json, "aps"), 0), "ssid")
                   ->valuestring) == "boat\"\\name");
    cJSON_Delete(json);
    const std::string valid = R"({"ssid":"boat","password":"password123"})";
    assert(request("/submit", valid, "bad").status == "403");
    assert(request("/submit", valid, secret, "evil.example").status == "403");
    assert(!succeeded(request("/submit", R"({"ssid":"boat","password":42})")));
    assert(!succeeded(request("/submit", R"({"ssid":"boat","password":"short"})")));
    assert(!succeeded(
        request("/submit", R"({"ssid":"boat","password":"password","x":{"nested":{}}})")));
    assert(!succeeded(request("/submit", valid + "junk")));
    assert(ap.connects == 0);
    ap.connect_ok = false;
    assert(!succeeded(request("/submit", valid)));
    assert(ap.saves == 0);
    begin();
    ap.during_connect = [&]() { now_us = OrbitWifiSession::kLifetimeUs; };
    assert(!succeeded(request("/submit", valid)));
    assert(ap.saves == 0);
    begin();
    ap.during_connect = [&]() { ap.CancelOrbitSetup(); };
    assert(!succeeded(request("/submit", valid)));
    assert(ap.saves == 0);
    begin();
    ap.save_ok = false;
    assert(!succeeded(request("/submit", valid)));
    assert(ap.OrbitSetupResult() == OrbitWifiSession::Result::Active);
    begin();
    assert(succeeded(request("/submit", valid)));
    int saves = ap.saves;
    assert(!succeeded(request("/submit", valid)));
    assert(ap.saves == saves);
    begin();
    assert(succeeded(request("/exit")));
    assert(ap.OrbitSetupResult() == OrbitWifiSession::Result::Cancelled);
    ap.DiscardOrbitSetup();
    assert(ap.orbit_password_.empty());
    ap.PrepareOrbitSetup(secret);  // cancellation before HTTP/AP starts
    ap.CancelOrbitSetup();
    ap.DiscardOrbitSetup();
    assert(ap.orbit_password_.empty());
    assert(ap.OrbitSetupResult() == OrbitWifiSession::Result::Cancelled);
}
