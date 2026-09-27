#include "orbit_wifi_session.h"
#include <atomic>
#include <cassert>
#include <thread>
int main() {
    using S = OrbitWifiSession;
    S s;
    int saves = 0;
    auto save = [&]() {
        ++saves;
        return true;
    };
    assert(!s.Commit(1, save));
    s.Begin(10);
    assert(s.Poll(10 + S::kLifetimeUs - 1) == S::Result::Active);
    assert(!s.Commit(10 + S::kLifetimeUs, save));
    assert(s.Poll(11 + S::kLifetimeUs) == S::Result::Expired);
    s.Begin(20);
    s.Cancel();
    assert(!s.Commit(21, save));
    s.Begin(30);
    assert(!s.Commit(31, []() { return false; }));
    assert(s.Poll(32) == S::Result::Active);
    assert(s.Commit(33, save));
    assert(!s.Commit(34, save));
    s.Cancel();
    assert(s.Poll(999999999) == S::Result::Saved);
    assert(saves == 1);
    assert(S::ValidCredentials(std::string(32, 's'), std::string(63, 'p')));
    assert(S::ValidCredentials("boat", ""));
    assert(S::ValidCredentials("boat", std::string(64, 'a')));
    assert(!S::ValidCredentials("boat", std::string(64, 'z')));
    assert(!S::ValidCredentials(std::string(33, 's'), "password"));
    assert(!S::ValidCredentials("", "password"));
    assert(!S::ValidCredentials("boat", "short"));
    assert(!S::ValidCredentials(std::string("a\0b", 3), "password"));
    // A racing cancel can win or lose, but commit may happen only once.
    for (int i = 0; i < 100; i++) {
        s.Begin(0);
        std::atomic<int> calls{0};
        std::thread a([&]() { s.Cancel(); });
        std::thread b([&]() {
            s.Commit(1, [&]() {
                ++calls;
                return true;
            });
        });
        a.join();
        b.join();
        assert(calls <= 1);
        assert(!s.Commit(2, save));
    }
}
