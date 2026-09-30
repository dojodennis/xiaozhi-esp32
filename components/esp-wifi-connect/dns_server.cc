#include "dns_server.h"
#include <esp_log.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include "orbit_dns_reply.h"

#define TAG "DnsServer"

DnsServer::DnsServer() : stopped_(xSemaphoreCreateBinary()) {}

DnsServer::~DnsServer() {
    Stop();
    if (stopped_)
        vSemaphoreDelete(stopped_);
}

void DnsServer::Start(esp_ip4_addr_t gateway) {
    Stop();
    if (!stopped_)
        return;
    gateway_ = gateway;
    fd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd_ < 0)
        return;
    // Timeout also bounds shutdown if the platform does not wake UDP recv on shutdown.
    const timeval timeout{0, 250000};
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port_);
    if (setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0 ||
        bind(fd_, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) != 0) {
        close(fd_);
        fd_ = -1;
        return;
    }
    running_ = true;
    if (xTaskCreate(
            [](void* arg) {
                auto* self = static_cast<DnsServer*>(arg);
                self->Run();
                // Last access to self. Stop joins this signal before freeing the object.
                xSemaphoreGive(self->stopped_);
                vTaskDelete(nullptr);
            },
            "DnsServerTask", 4096, this, 5, &task_handle_) != pdPASS) {
        running_ = false;
        task_handle_ = nullptr;
        close(fd_);
        fd_ = -1;
    }
}

void DnsServer::Stop() {
    running_ = false;
    if (task_handle_) {
        shutdown(fd_, SHUT_RDWR);
        xSemaphoreTake(stopped_, portMAX_DELAY);
        task_handle_ = nullptr;
    }
    // Never close/reuse the descriptor while the worker may still be using it.
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

void DnsServer::Run() {
    uint8_t buffer[513];  // One extra byte detects oversized/truncated datagrams.
    while (running_) {
        sockaddr_in client_addr{};
        socklen_t client_addr_len = sizeof(client_addr);
        const int length = recvfrom(fd_, buffer, sizeof(buffer), 0,
                                    reinterpret_cast<sockaddr*>(&client_addr), &client_addr_len);
        if (!running_)
            break;
        if (length <= 0)
            continue;
        const size_t response = OrbitDnsReply(buffer, length, 512, &gateway_.addr);
        if (response != 0)
            sendto(fd_, buffer, response, 0, reinterpret_cast<sockaddr*>(&client_addr),
                   client_addr_len);
    }
}
