#ifndef PROVISIONS_TIMER_STORE_H_
#define PROVISIONS_TIMER_STORE_H_
#include "provisions_timers.h"
namespace provisions::timers {
// A separate persistent API; never reads or rewrites the voice outbox namespace.
class Store {
public:
    enum class LoadResult { Empty, Present, Fault };
    virtual ~Store() = default;
    virtual LoadResult Load(Record& record) = 0;
    virtual bool Save(const Record& record) = 0;
    virtual bool Erase(const Record& expected) = 0;
};
class NvsStore final : public Store {
public:
    LoadResult Load(Record& record) override;
    bool Save(const Record& record) override;
    bool Erase(const Record& expected) override;
};
}  // namespace provisions::timers
#endif
