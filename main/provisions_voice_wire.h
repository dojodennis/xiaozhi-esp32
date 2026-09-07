#ifndef PROVISIONS_VOICE_WIRE_H
#define PROVISIONS_VOICE_WIRE_H
#include <cJSON.h>
#include <string>
#include "provisions_voice_recorder.h"

namespace provisions {
bool ParseVoiceContext(const cJSON* value, VoiceContext& output);
bool ParseVoiceReceipt(const cJSON* value, VoiceCaptureReceipt& output);
bool ParseVoiceId(const char* value, VoiceId& output);
std::string VoiceIdText(const VoiceId& id);
std::string VoiceCaptureStart(const VoiceReplay& replay, const std::string& session, uint32_t turn,
                              bool deferred);
}  // namespace provisions
#endif
