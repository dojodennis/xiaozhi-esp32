#ifndef _PROVISIONS_ENDPOINT_POLICY_H_
#define _PROVISIONS_ENDPOINT_POLICY_H_

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
bool IsAllowedHealthUrl(const std::string& url);
bool IsValidDeviceToken(const std::string& token);
bool IsCanonicalUuid(const std::string& value);

}  // namespace ProvisionsEndpointPolicy

#endif  // _PROVISIONS_ENDPOINT_POLICY_H_
