import importlib.util
import json
import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BOARD_DIR = ROOT / "main/boards/m5stack/provisions-core-s3"
SPEC = importlib.util.spec_from_file_location("provisions_build", ROOT / "scripts/build.py")
build = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(build)


class ProvisionsCoreS3ProfileTests(unittest.TestCase):
    def test_board_is_unique_and_resolves_to_its_kconfig_symbol(self):
        config = json.loads((BOARD_DIR / "config.json").read_text(encoding="utf-8"))
        self.assertEqual(config["manufacturer"], "m5stack")
        self.assertEqual(config["type"], "provisions-kitchen-helper-core-s3")
        self.assertEqual(config["builds"][0]["name"], "provisions-kitchen-helper-core-s3")
        self.assertEqual(
            build._resolve_board_config("m5stack/provisions-core-s3", "esp32s3", []),
            "CONFIG_BOARD_TYPE_M5STACK_PROVISIONS_CORE_S3",
        )

    def test_release_logging_cannot_emit_debug_request_headers(self):
        config = json.loads((BOARD_DIR / "config.json").read_text(encoding="utf-8"))
        sdkconfig = set(config["builds"][0]["sdkconfig_append"])
        self.assertIn("CONFIG_LOG_DEFAULT_LEVEL_INFO=y", sdkconfig)
        self.assertIn("CONFIG_LOG_MAXIMUM_LEVEL_INFO=y", sdkconfig)
        self.assertFalse(any("DEBUG=y" in item or "VERBOSE=y" in item for item in sdkconfig))

    def test_board_keeps_core_hardware_and_only_adds_active_low_talk(self):
        source = (BOARD_DIR / "provisions_core_s3.cc").read_text(encoding="utf-8")
        config = (BOARD_DIR / "config.h").read_text(encoding="utf-8")
        self.assertEqual(source.count("DECLARE_BOARD("), 1)
        self.assertIn("Button talk_button_", source)
        self.assertIn("talk_button_(TALK_BUTTON_GPIO, false)", source)
        self.assertIn("OnPressDown", source)
        self.assertIn("StartListening", source)
        self.assertIn("OnPressUp", source)
        self.assertIn("StopListening", source)
        self.assertIn("#define TALK_BUTTON_GPIO GPIO_NUM_8", config)
        self.assertIn("CoreS3AudioCodec", source)
        self.assertIn("GetBatteryLevel", source)
        self.assertNotIn("Camera", source)
        self.assertNotIn("Touch", source)
        self.assertNotIn("WakeWord", source)

    def test_profile_uses_local_assets_and_exact_preview_bootstrap(self):
        config = json.loads((BOARD_DIR / "config.json").read_text(encoding="utf-8"))
        sdkconfig = config["builds"][0]["sdkconfig_append"]
        self.assertIn(
            'CONFIG_OTA_URL="https://app.provisions-app.com/kitchen-helper/preview/v1/bootstrap"',
            sdkconfig,
        )
        serialized = json.dumps(config)
        self.assertNotIn("wifi_ssid", serialized.casefold())
        self.assertNotIn("wifi_password", serialized.casefold())
        self.assertNotIn("device_token", serialized.casefold())

        cmake = (ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn('set(BOARD_DIR "m5stack/provisions-core-s3")', cmake)
        self.assertIn("DEFAULT_EMOJI_COLLECTION noto-color-emoji_64", cmake)
        self.assertIn('"boards/m5stack/core-s3/cores3_audio_codec.cc"', cmake)
        self.assertIn('"provisions_endpoint_policy.cc"', cmake)
        self.assertIn(
            "NOT CONFIG_BOARD_TYPE_M5STACK_PROVISIONS_CORE_S3",
            cmake,
        )


class ProvisionsGatewayIntegrationTests(unittest.TestCase):
    def test_gateway_handshake_and_server_hello_are_fail_closed(self):
        websocket = (ROOT / "main/protocols/websocket_protocol.cc").read_text(encoding="utf-8")
        ota = (ROOT / "main/ota.cc").read_text(encoding="utf-8")
        self.assertIn('SetHeader("Authorization", token.c_str())', websocket)
        self.assertIn('SetHeader("Protocol-Version", std::to_string(version_).c_str())', websocket)
        self.assertIn('SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str())', websocket)
        self.assertIn('SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str())', websocket)
        self.assertIn('SetHeader("X-Provisions-Boot-Id", SystemInfo::GetBootId().c_str())', websocket)
        self.assertIn("esp_app_get_description()->version", websocket)
        self.assertIn('cJSON_GetObjectItem(root, "provisions")', websocket)
        self.assertIn('cJSON_GetObjectItem(provisions, "authenticated")', websocket)
        self.assertIn("IsCanonicalUuid", websocket)
        self.assertIn('strcmp(format->valuestring, "opus")', websocket)
        self.assertIn("sample_rate->valuedouble != 24000", websocket)
        self.assertIn("channels->valuedouble != 1", websocket)
        self.assertIn("frame_duration->valuedouble != 60", websocket)
        self.assertIn("RejectServerHello", websocket)
        self.assertIn('http->SetHeader("Protocol-Version", "1")', ota)

    def test_gateway_heartbeat_and_thin_server_capabilities_are_frozen(self):
        websocket = (ROOT / "main/protocols/websocket_protocol.cc").read_text(encoding="utf-8")
        self.assertIn(
            'return SendText("{\\\"session_id\\\":\\\"" + session_id_ + "\\\",\\\"type\\\":\\\"ping\\\"}")',
            websocket,
        )
        self.assertIn('strcmp(type->valuestring, "pong") == 0', websocket)
        self.assertIn("cJSON_GetArraySize(root) != 2", websocket)
        self.assertIn('cJSON_AddBoolToObject(features, "mcp", false)', websocket)

    def test_provisions_application_frames_are_strict_and_private(self):
        application = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        self.assertIn('strcmp(type->valuestring, "provisions") != 0', application)
        self.assertIn('strcmp(type->valuestring, "tts") != 0', application)
        self.assertIn("Rejecting unsupported Provisions gateway frame type", application)
        self.assertIn('{"session_id", "type", "state", "text", "receipt_id"}', application)
        self.assertIn('strcmp(state->valuestring, "working") == 0', application)
        self.assertIn('strcmp(text->valuestring, "Working") == 0', application)
        self.assertIn('strcmp(state->valuestring, "result") == 0', application)
        self.assertIn('text == "Found"', application)
        self.assertIn('text == "Check app"', application)
        self.assertIn('protocol_->session_id() == session->valuestring', application)
        self.assertIn('HasExactKeys(root, {"session_id", "type", "state"})', application)
        self.assertIn("IsBoundedTtsText", application)
        self.assertIn("#if !CONFIG_PROVISIONS_GATEWAY_REQUIRED\n                auto text", application)

    def test_ready_working_watchdog_and_fast_release_are_explicit(self):
        application = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        header = (ROOT / "main/application.h").read_text(encoding="utf-8")
        self.assertIn("manual_listening_requested_.store(true)", application)
        self.assertIn("manual_listening_requested_.store(false)", application)
        self.assertGreaterEqual(application.count("!manual_listening_requested_.load()"), 3)
        self.assertIn(
            "#if CONFIG_PROVISIONS_GATEWAY_REQUIRED\n"
            "    } else if (state == kDeviceStateConnecting)",
            application,
        )
        self.assertIn("SetProvisionsResponsePending(true)", application)
        self.assertIn('return provisions_response_pending_.load() ? "Working" : "Ready"', application)
        self.assertIn("Ignoring Talk while the previous request is working", application)
        self.assertIn("SendGatewayHeartbeat", application)
        self.assertIn("IsGatewayHeartbeatExpired", application)
        self.assertIn("kProvisionsMaximumReconnectAttempts = 5", application)
        self.assertIn("std::atomic<bool> manual_listening_requested_", header)
        self.assertIn("std::atomic<bool> provisions_response_pending_", header)

    def test_preview_boot_and_rollback_validation_are_bounded(self):
        application = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        self.assertIn("const int MAX_RETRY = 3", application)
        self.assertIn("int retry_delay = 1", application)
        mark = application.index("ota_->MarkCurrentVersionValid();")
        authenticated_guard = application.index("!protocol_->IsAudioChannelOpened()")
        reset = application.index("ota_.reset();")
        self.assertLess(authenticated_guard, mark)
        self.assertLess(mark, reset)

    def test_exact_preview_paths_and_no_legacy_audio_path(self):
        policy = (ROOT / "main/provisions_endpoint_policy.cc").read_text(encoding="utf-8")
        expected = (
            "wss://app.provisions-app.com/kitchen-helper/preview/v1/device",
            "https://app.provisions-app.com/kitchen-helper/preview/v1/bootstrap",
            "https://app.provisions-app.com/kitchen-helper/preview/v1/firmware/manifest.json",
            "https://app.provisions-app.com/kitchen-helper/preview/v1/health",
        )
        for endpoint in expected:
            self.assertIn(endpoint, policy)
        self.assertNotIn("/kitchen-helper/preview/v1/audio", policy)
        self.assertIn("parsed.host == kExpectedHost && parsed.port == 443", policy)
        self.assertIn("IsValidDeviceToken", policy)


class ProvisionsEndpointPolicyCompileTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "host C++ compiler is unavailable")
    def test_endpoint_credential_and_uuid_policy(self):
        test_source = textwrap.dedent(
            r"""
            #include "provisions_endpoint_policy.h"
            #include <cassert>
            #include <string>

            int main() {
                using namespace ProvisionsEndpointPolicy;
                const std::string hash(64, 'a');
                assert(IsAllowedBootstrapUrl(BootstrapUrl()));
                assert(IsAllowedBootstrapUrl(
                    "HTTPS://APP.PROVISIONS-APP.COM:443/kitchen-helper/preview/v1/bootstrap"));
                assert(IsAllowedWebsocketUrl(WebsocketUrl()));
                assert(IsAllowedOtaManifestUrl(OtaManifestUrl()));
                assert(IsAllowedHealthUrl(HealthUrl()));
                assert(IsAllowedFirmwareUrl(
                    "https://app.provisions-app.com/kitchen-helper/preview/v1/firmware/"
                    "provisions-kitchen-helper-core-s3/1.0.0/" + hash + ".bin"));

                assert(!IsAllowedWebsocketUrl(
                    "wss://app.provisions-app.com/kitchen-helper/preview/v1/audio"));
                assert(!IsAllowedWebsocketUrl(
                    "wss://app.provisions-app.com.evil.test/kitchen-helper/preview/v1/device"));
                assert(!IsAllowedWebsocketUrl(
                    "wss://app.provisions-app.com@evil.test/kitchen-helper/preview/v1/device"));
                assert(!IsAllowedWebsocketUrl(
                    "wss://app.provisions-app.com:444/kitchen-helper/preview/v1/device"));
                assert(!IsAllowedWebsocketUrl(
                    "wss://app.provisions-app.com/kitchen-helper/preview/v1/device?x=1"));
                assert(!IsAllowedFirmwareUrl(
                    "https://app.provisions-app.com/kitchen-helper/preview/v1/firmware/"
                    "other-board/1.0.0/" + hash + ".bin"));
                assert(!IsAllowedFirmwareUrl(
                    "https://app.provisions-app.com/kitchen-helper/preview/v1/firmware/"
                    "provisions-kitchen-helper-core-s3/1.0.0/not-a-sha.bin"));

                const std::string token =
                    "pvd1_123e4567-e89b-42d3-a456-426614174000." + std::string(43, 'A');
                assert(IsValidDeviceToken(token));
                assert(!IsValidDeviceToken("Bearer " + token));
                assert(!IsValidDeviceToken(
                    "pvd1_123e4567-e89b-42d3-a456-426614174000." + std::string(42, 'A')));
                assert(!IsValidDeviceToken(
                    "pvd1_123e4567-e89b-42d3-a456-426614174000." + std::string(44, 'A')));
                assert(!IsValidDeviceToken(
                    "pvd1_123e4567-e89b-42d3-a456-426614174000." + std::string(42, 'A') + "."));
                assert(!IsValidDeviceToken(
                    "pvd1_123e4567-e89b-42d3-a456-426614174000." + std::string(42, 'A') + "+"));
                assert(!IsValidDeviceToken(
                    "pvd1_123e4567-e89b-02d3-a456-426614174000." + std::string(43, 'A')));
                assert(!IsValidDeviceToken("pvd1_bad\r\nInjected"));
                assert(IsCanonicalUuid("123e4567-e89b-42d3-a456-426614174000"));
                assert(!IsCanonicalUuid("123e4567-e89b-02d3-a456-426614174000"));
                assert(!IsCanonicalUuid("not-a-uuid"));
            }
            """
        )
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary = Path(temporary_directory)
            source = temporary / "policy_test.cc"
            executable = temporary / "policy_test"
            source.write_text(test_source, encoding="utf-8")
            command = [
                shutil.which("c++"),
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Werror",
                f'-DCONFIG_PROVISIONS_PREVIEW_HOST="app.provisions-app.com"',
                f'-DCONFIG_PROVISIONS_PREVIEW_PATH_PREFIX="/kitchen-helper/preview/v1/"',
                "-DCONFIG_PROVISIONS_PREVIEW_WEBSOCKET_URL=\"wss://app.provisions-app.com/kitchen-helper/preview/v1/device\"",
                "-DCONFIG_OTA_URL=\"https://app.provisions-app.com/kitchen-helper/preview/v1/bootstrap\"",
                "-I",
                str(ROOT / "main"),
                str(ROOT / "main/provisions_endpoint_policy.cc"),
                str(source),
                "-o",
                str(executable),
            ]
            subprocess.run(command, check=True, cwd=ROOT)
            subprocess.run([str(executable)], check=True, cwd=ROOT)


if __name__ == "__main__":
    unittest.main()
