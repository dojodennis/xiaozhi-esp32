#ifndef PROVISIONS_WEBSOCKET_H
#define PROVISIONS_WEBSOCKET_H

#include <functional>
#include <memory>
#include <string>

// StopWatch Wi-Fi transport. A single I/O task owns TLS, framing and disposal.
// Close and queued control messages never wait on network I/O on their caller.
class ProvisionsWebSocket {
public:
    ProvisionsWebSocket();
    ~ProvisionsWebSocket();
    void SetHeader(const char* key, const char* value);
    void OnData(std::function<void(const char*, size_t, bool)> callback);
    void OnDisconnected(std::function<void()> callback);
    bool Connect(const char* uri);
    bool IsConnected() const;
    int GetLastError() const;
    bool Send(const std::string& text);
    bool Send(const void* data, size_t size, bool binary);
    bool SendAsync(const std::string& text);
    void Close();

private:
    struct State;
    std::shared_ptr<State> state_;
    bool Enqueue(const void* data, size_t size, bool binary, bool wait);
};
#endif
