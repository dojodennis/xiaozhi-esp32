#ifndef PROVISIONS_HARDWARE_FACTS_H_
#define PROVISIONS_HARDWARE_FACTS_H_
#include <cstdint>
#include <string>
// Bench facts the ring can otherwise only report down a serial cable: whether
// the touch panel answers on the bus, and how long the microphone took to
// deliver its first chunk after a press. They ride the device hello so they can
// be read from the gateway log, and nothing on either side branches on them.
namespace provisions::hardware {
void SetTouchProbe(const char* result);
std::string TouchProbe();
void NoteMicReadyMs(int milliseconds);
// -1 until a press has been measured.
int MicReadyMs();
// PM1 REG_WAKE_SRC (0x05) bits read at boot: why PM1 last powered or reset the
// ESP32-S3. -1 when the register could not be read.
void SetPm1Wake(uint8_t source);
int Pm1Wake();
}  // namespace provisions::hardware
#endif  // PROVISIONS_HARDWARE_FACTS_H_
