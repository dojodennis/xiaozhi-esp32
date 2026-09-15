#ifndef M5STACK_STOPWATCH_CST820_TOUCH_H_
#define M5STACK_STOPWATCH_CST820_TOUCH_H_

#include <cstdint>

#include <driver/i2c_master.h>

class StopwatchCst820Touch {
public:
    ~StopwatchCst820Touch();

    bool Begin(i2c_master_bus_handle_t bus, uint8_t address = 0x15);
    // x/y are the controller's raw point from the same frame, meaningful only
    // while pressed. They are half resolution (0..233, per M5GFX's StopWatch
    // touch calibration) in the panel's orientation (config.h: no mirror, no
    // swap); ProvisionsStopwatchOrbit::RawTouchToDisplay maps them to pixels.
    bool ReadPressed(bool& pressed, int& x, int& y);

private:
    bool ReadRegister(uint8_t reg, uint8_t* data, size_t size);

    i2c_master_dev_handle_t device_ = nullptr;
};

#endif  // M5STACK_STOPWATCH_CST820_TOUCH_H_
