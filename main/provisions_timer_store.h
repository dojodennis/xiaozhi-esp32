#ifndef PROVISIONS_TIMER_STORE_H_
#define PROVISIONS_TIMER_STORE_H_
#include "provisions_timers.h"
namespace provisions::timers {
// A separate persistent API; never reads or rewrites the voice outbox namespace.
class Store {
public:
    enum class LoadResult { Empty, Present, Fault };
    virtual ~Store() = default;
    virtual LoadResult Load(DurableSlot& slot) = 0;
    virtual bool Transition(const DurableSlot& expected, const DurableSlot& desired) = 0;
    virtual bool Erase(const DurableSlot& expected) = 0;
};
class NvsStore final : public Store {
public:
    LoadResult Load(DurableSlot& slot) override;
    bool Transition(const DurableSlot& expected, const DurableSlot& desired) override;
    bool Erase(const DurableSlot& expected) override;
};
}  // namespace provisions::timers
#endif
