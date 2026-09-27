#pragma once
#include <cassert>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "orbit_wifi_session.h"
using esp_err_t = int;
constexpr int ESP_OK = 0, HTTPD_403_FORBIDDEN = 403;
#define ESP_ERROR_CHECK(x) assert((x) == ESP_OK)
enum httpd_method_t { HTTP_GET, HTTP_POST };
struct httpd_req_t {
    void* user_ctx = nullptr;
    size_t content_len = 0, offset = 0;
    std::string body, response, type, status;
    std::map<std::string, std::string> headers;
};
struct httpd_uri_t {
    const char* uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t*);
    void* user_ctx;
};
using httpd_handle_t = int;
struct httpd_config_t {
    int max_uri_handlers = 0, recv_wait_timeout = 0, send_wait_timeout = 0;
};
#define HTTPD_DEFAULT_CONFIG() \
    httpd_config_t {}
inline std::map<std::string, httpd_uri_t> routes;
inline int httpd_start(int* server, httpd_config_t*) {
    *server = 1;
    routes.clear();
    return 0;
}
inline int httpd_register_uri_handler(int, httpd_uri_t* r) {
    routes[r->uri] = *r;
    return 0;
}
inline int httpd_req_get_hdr_value_str(httpd_req_t* r, const char* name, char* out,
                                       size_t capacity) {
    auto it = r->headers.find(name);
    if (it == r->headers.end() || it->second.size() >= capacity)
        return -1;
    strcpy(out, it->second.c_str());
    return 0;
}
inline int httpd_resp_set_type(httpd_req_t* r, const char* type) {
    r->type = type;
    return 0;
}
inline int httpd_resp_set_status(httpd_req_t* r, const char* status) {
    r->status = status;
    return 0;
}
inline int httpd_resp_set_hdr(httpd_req_t*, const char*, const char*) { return 0; }
inline int httpd_resp_send(httpd_req_t* r, const char* data, size_t size) {
    r->response.assign(data, size);
    return 0;
}
inline int httpd_resp_sendstr(httpd_req_t* r, const char* data) {
    r->response = data;
    return 0;
}
inline int httpd_resp_send_err(httpd_req_t* r, int code, const char* msg) {
    r->status = std::to_string(code);
    r->response = msg;
    return 0;
}
inline int httpd_req_recv(httpd_req_t* r, char* out, size_t size) {
    size_t n = std::min({size, r->body.size() - r->offset, size_t{3}});
    if (n == 0)
        return -1;
    memcpy(out, r->body.data() + r->offset, n);
    r->offset += n;
    return static_cast<int>(n);
}
struct wifi_ap_record_t {
    unsigned char ssid[33]{};
};
class WifiConfigurationAp {
public:
    OrbitWifiSession orbit_session_;
    std::string orbit_password_;
    int server_ = 0, event_group_ = 0, saves = 0, connects = 0;
    bool connect_ok = true, save_ok = true;
    std::function<void()> during_connect;
    void PrepareOrbitSetup(const std::string&);
    void CancelOrbitSetup();
    void DiscardOrbitSetup();
    OrbitWifiSession::Result OrbitSetupResult();
    void StartOrbitWebServer();
    bool ConnectToWifi(const std::string&, const std::string&) {
        ++connects;
        if (during_connect)
            during_connect();
        return connect_ok;
    }
    bool Save(const std::string&, const std::string&) {
        ++saves;
        return save_ok;
    }
    std::vector<wifi_ap_record_t> GetAccessPoints() {
        wifi_ap_record_t ap;
        strcpy(reinterpret_cast<char*>(ap.ssid), "boat\"\\name");
        return {ap};
    }
};
