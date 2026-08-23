#ifndef _PROVISIONS_ENDPOINT_POLICY_H_
#define _PROVISIONS_ENDPOINT_POLICY_H_

#include <array>
#include <cstdint>
#include <string>

namespace ProvisionsEndpointPolicy {

const char* BootstrapUrl();
const char* WebsocketUrl();
const char* OtaManifestUrl();
const char* HealthUrl();

bool IsAllowedBootstrapUrl(const std::string& url);
bool IsAllowedWebsocketUrl(const std::string& url);
bool IsAllowedOtaManifestUrl(const std::string& url);
bool IsAllowedFirmwareUrl(const std::string& url);
bool FirmwareUrlMatchesVersion(const std::string& url, const std::string& version);
bool ExtractFirmwareVersion(const std::string& url, std::string& version);
bool ExtractFirmwareSha256(const std::string& url, std::array<uint8_t, 32>& digest);
bool IsNewerFirmwareVersion(const std::string& current_version,
                            const std::string& candidate_version);
bool IsApprovedFirmwareImageVersion(const std::string& url,
                                    const std::string& current_version,
                                    const std::string& embedded_version);
bool IsAllowedHealthUrl(const std::string& url);
bool IsValidDeviceToken(const std::string& token);
bool IsCanonicalUuid(const std::string& value);

}  // namespace ProvisionsEndpointPolicy

#endif  // _PROVISIONS_ENDPOINT_POLICY_H_
