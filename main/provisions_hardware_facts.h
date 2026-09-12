#ifndef PROVISIONS_HARDWARE_FACTS_H_
#define PROVISIONS_HARDWARE_FACTS_H_
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
}  // namespace provisions::hardware
#endif  // PROVISIONS_HARDWARE_FACTS_H_
