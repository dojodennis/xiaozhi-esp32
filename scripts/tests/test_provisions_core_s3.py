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
FULL_PROFILE = "provisions-kitchen-helper-core-s3"
LITE_PROFILE = "provisions-kitchen-helper-core-s3-lite"
PROFILE_SYMBOLS = {
    FULL_PROFILE: "CONFIG_BOARD_TYPE_M5STACK_PROVISIONS_CORE_S3",
    LITE_PROFILE: "CONFIG_BOARD_TYPE_M5STACK_PROVISIONS_CORE_S3_LITE",
}
SPEC = importlib.util.spec_from_file_location("provisions_build", ROOT / "scripts/build.py")
build = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(build)


class ProvisionsCoreS3ProfileTests(unittest.TestCase):
    def test_board_is_unique_and_resolves_to_its_kconfig_symbol(self):
        config = json.loads((BOARD_DIR / "config.json").read_text(encoding="utf-8"))
        self.assertEqual(config["manufacturer"], "m5stack")
        self.assertEqual(config["type"], FULL_PROFILE)
        self.assertEqual({item["name"] for item in config["builds"]}, set(PROFILE_SYMBOLS))
        for profile in config["builds"]:
            name = profile["name"]
            sdkconfig = set(profile["sdkconfig_append"])
            self.assertIn(f"{PROFILE_SYMBOLS[name]}=y", sdkconfig)
            self.assertTrue(
                all(
                    f"{symbol}=y" not in sdkconfig
                    for other_name, symbol in PROFILE_SYMBOLS.items()
                    if other_name != name
                )
            )
            self.assertEqual(
                build._resolve_board_config(
                    "m5stack/provisions-core-s3",
                    "esp32s3",
                    profile["sdkconfig_append"],
                    variant_name=name,
                ),
                PROFILE_SYMBOLS[name],
            )

    def test_release_logging_cannot_emit_debug_request_headers(self):
        config = json.loads((BOARD_DIR / "config.json").read_text(encoding="utf-8"))
        for profile in config["builds"]:
            sdkconfig = set(profile["sdkconfig_append"])
            self.assertIn("CONFIG_LOG_DEFAULT_LEVEL_INFO=y", sdkconfig)
            self.assertIn("CONFIG_LOG_MAXIMUM_LEVEL_INFO=y", sdkconfig)
            self.assertFalse(
                any("DEBUG=y" in item or "VERBOSE=y" in item for item in sdkconfig)
            )

    def test_developer_profile_explicitly_resets_pilot_security_choices(self):
        config = json.loads((BOARD_DIR / "config.json").read_text(encoding="utf-8"))
        required = {
                "CONFIG_SECURE_BOOT=n",
                "CONFIG_SECURE_BOOT_V2_ENABLED=n",
                "CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME=n",
                "CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=n",
                "CONFIG_SECURE_FLASH_ENC_ENABLED=n",
                "CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE=n",
                "CONFIG_NVS_ENCRYPTION=n",
                "CONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH_ENC=n",
            }
        for profile in config["builds"]:
            self.assertTrue(required.issubset(set(profile["sdkconfig_append"])))

    def test_board_keeps_shared_hardware_and_profile_guards_external_talk_pin(self):
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
        self.assertIn("#define TALK_BUTTON_GPIO GPIO_NUM_1", config)
        self.assertIn("PROVISIONS_NOMINAL_BATTERY_MAH 500", config)
        self.assertIn("PROVISIONS_NOMINAL_BATTERY_MAH 200", config)
        self.assertIn("PROVISIONS_HAS_DIN_BASE true", config)
        self.assertIn("PROVISIONS_HAS_DIN_BASE false", config)
        self.assertIn("CoreS3AudioCodec", source)
        self.assertIn("GetBatteryLevel", source)
        self.assertIn("kCameraResetHeld = 0b10001110", source)
        self.assertIn("kDisplayResetHeld = 0b10001100", source)
        self.assertIn('"PROVISIONS_SIGNED_HARDWARE_IDENTITY=" BOARD_NAME', source)
        self.assertNotIn('#include "camera', source.casefold())
        self.assertNotIn("GetCamera", source)
        self.assertNotIn("new Esp32Camera", source)
        self.assertNotIn("new EspVideo", source)
        self.assertNotIn("Touch", source)
        self.assertNotIn("WakeWord", source)

    def test_profile_uses_local_assets_and_exact_preview_bootstrap(self):
        config = json.loads((BOARD_DIR / "config.json").read_text(encoding="utf-8"))
        for profile in config["builds"]:
            sdkconfig = profile["sdkconfig_append"]
            self.assertIn(
                'CONFIG_OTA_URL="https://app.provisions-app.com/kitchen-helper/preview/v1/bootstrap"',
                sdkconfig,
            )
            self.assertIn("CONFIG_CAMERA_GC0308=n", sdkconfig)
            self.assertIn("CONFIG_ESP_VIDEO_ENABLE_DVP_VIDEO_DEVICE=n", sdkconfig)
        serialized = json.dumps(config)
        self.assertNotIn("wifi_ssid", serialized.casefold())
        self.assertNotIn("wifi_password", serialized.casefold())
        self.assertNotIn("device_token", serialized.casefold())

        cmake = (ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn('set(BOARD_DIR "m5stack/provisions-core-s3")', cmake)
        self.assertEqual(cmake.count('set(BOARD_DIR "m5stack/provisions-core-s3")'), 2)
        self.assertIn(
            'set(PROVISIONS_CORE_S3_BOARD_NAME "provisions-kitchen-helper-core-s3")',
            cmake,
        )
        self.assertIn(
            'set(PROVISIONS_CORE_S3_BOARD_NAME "provisions-kitchen-helper-core-s3-lite")',
            cmake,
        )
        self.assertIn(
            "NOT BOARD_NAME STREQUAL PROVISIONS_CORE_S3_BOARD_NAME", cmake
        )
        self.assertIn("DEFAULT_EMOJI_COLLECTION noto-color-emoji_64", cmake)
        self.assertIn('"boards/m5stack/core-s3/cores3_audio_codec.cc"', cmake)
        self.assertIn('"provisions_endpoint_policy.cc"', cmake)
        self.assertIn(
            "NOT CONFIG_BOARD_TYPE_M5STACK_PROVISIONS_CORE_S3",
            cmake,
        )
        self.assertIn(
            "NOT CONFIG_BOARD_TYPE_M5STACK_PROVISIONS_CORE_S3_LITE",
            cmake,
        )

    def test_pilot_profile_requires_signed_boot_encrypted_flash_and_nvs(self):
        config = json.loads(
            (BOARD_DIR / "pilot_profile.json").read_text(encoding="utf-8")
        )
        required = {
            "CONFIG_BOOTLOADER_SKIP_VALIDATE_ALWAYS=n",
            "CONFIG_SECURE_BOOT=y",
            "CONFIG_SECURE_BOOT_V2_ENABLED=y",
            "CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME=y",
            "CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=n",
            "CONFIG_SECURE_FLASH_ENC_ENABLED=y",
            "CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE=y",
            "CONFIG_NVS_ENCRYPTION=y",
            "CONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH_ENC=y",
            'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions/provisions/16m.csv"',
        }
        self.assertEqual({item["name"] for item in config["builds"]}, set(PROFILE_SYMBOLS))
        for profile in config["builds"]:
            sdkconfig = set(profile["sdkconfig_append"])
            self.assertTrue(required.issubset(sdkconfig))
            self.assertIn(f"{PROFILE_SYMBOLS[profile['name']]}=y", sdkconfig)
            self.assertTrue(
                all(
                    f"{symbol}=y" not in sdkconfig
                    for other_name, symbol in PROFILE_SYMBOLS.items()
                    if other_name != profile["name"]
                )
            )
            self.assertIn("CONFIG_CAMERA_GC0308=n", sdkconfig)
            self.assertIn("CONFIG_ESP_VIDEO_ENABLE_DVP_VIDEO_DEVICE=n", sdkconfig)
            self.assertFalse(
                any("SECURE_BOOT_SIGNING_KEY" in item for item in sdkconfig)
            )

        partition_table = (
            ROOT / "partitions/provisions/16m.csv"
        ).read_text(encoding="utf-8")
        self.assertIn("nvs_keys,   data, nvs_keys, 0x10000,  0x1000,   encrypted", partition_table)

    def test_factory_wifi_has_no_chef_setup_portal_or_plaintext_ssid_log(self):
        wifi_board = (ROOT / "main/boards/common/wifi_board.cc").read_text(
            encoding="utf-8"
        )
        application = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        self.assertIn('#if CONFIG_PROVISIONS_GATEWAY_REQUIRED\n'
                      '    // The pilot is factory-provisioned.', wifi_board)
        self.assertIn('config.ssid_prefix = "Provisions";', wifi_board)
        self.assertIn('config.show_ota_config = false;', wifi_board)
        self.assertIn('config.show_sleep_config = false;', wifi_board)
        self.assertIn('esp_log_level_set("WifiStation", ESP_LOG_WARN);', wifi_board)
        self.assertIn('esp_log_level_set("SsidManager", ESP_LOG_WARN);', wifi_board)
        self.assertIn('"Interactive WiFi configuration is disabled', wifi_board)
        self.assertIn('"Configured WiFi connection is still unavailable"', wifi_board)
        self.assertIn("WifiManager::GetInstance().IsConnected()", wifi_board)
        self.assertNotIn("provisions_wifi_retry", wifi_board)
        self.assertIn('#if CONFIG_PROVISIONS_GATEWAY_REQUIRED\n'
                      '                display->SetStatus("Connecting");', application)

    def test_ota_binds_signed_image_version_and_cleans_up_partial_downloads(self):
        ota = (ROOT / "main/ota.cc").read_text(encoding="utf-8")
        self.assertIn("IsApprovedFirmwareImageVersion(", ota)
        self.assertIn("new_app_info.magic_word != ESP_APP_DESC_MAGIC_WORD", ota)
        self.assertIn(
            "image_header.append(buffer + write_offset, static_cast<size_t>(ret));",
            ota,
        )
        self.assertIn("total_read + static_cast<size_t>(ret) > content_length", ota)
        self.assertIn("if (image_header_checked &&", ota)
        read_error = ota.index('ESP_LOGE(TAG, "Failed to read HTTP data: %s"')
        read_error_end = ota.index("#if CONFIG_PROVISIONS_GATEWAY_REQUIRED", read_error)
        self.assertIn("esp_ota_abort(update_handle);", ota[read_error:read_error_end])


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
        for receipt_text in (
            "Delivered",
            "On the way",
            "Recorded",
            "Choose one",
            "Need unit",
            "Ready to add",
            "Added",
            "Undone",
            "Not changed",
        ):
            self.assertIn(f'text == "{receipt_text}"', application)
        self.assertIn('protocol_->session_id() == session->valuestring', application)
        self.assertIn('HasExactKeys(root, {"session_id", "type", "state"})', application)
        self.assertIn("IsBoundedTtsText", application)

    def test_talk_release_is_a_hard_audio_upload_boundary(self):
        application = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        audio_service = (ROOT / "main/audio/audio_service.cc").read_text(
            encoding="utf-8"
        )
        audio_header = (ROOT / "main/audio/audio_service.h").read_text(
            encoding="utf-8"
        )

        stop_handler = application.split(
            "void Application::HandleStopListeningEvent()", 1
        )[1].split("void Application::HandleWakeWordDetectedEvent()", 1)[0]
        disable = stop_handler.index("audio_service_.EnableVoiceProcessing(false);")
        stop_frame = stop_handler.index("protocol_->SendStopListening();")
        idle = stop_handler.index("SetDeviceState(kDeviceStateIdle);", stop_frame)
        self.assertLess(disable, stop_frame)
        self.assertLess(stop_frame, idle)

        release_entrypoint = application.split("void Application::StopListening()", 1)[
            1
        ].split("void Application::HandleToggleChatEvent()", 1)[0]
        self.assertIn("manual_listening_requested_.store(false", release_entrypoint)
        self.assertIn("audio_service_.CloseVoiceUploadGate();", release_entrypoint)
        self.assertLess(
            release_entrypoint.index("audio_service_.CloseVoiceUploadGate();"),
            release_entrypoint.index(
                "xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING);"
            ),
        )
        self.assertIn('VoiceUploadGate voice_upload_gate_;', audio_header)
        self.assertIn("voice_upload_gate_.Close();", audio_service)
        self.assertIn("audio_send_queue_.clear();", audio_service)
        self.assertGreaterEqual(audio_service.count("voice_upload_gate_.Allows"), 3)
        send_loop = application.split("if (bits & MAIN_EVENT_SEND_AUDIO)", 1)[1].split(
            "if (bits & MAIN_EVENT_WAKE_WORD_DETECTED)", 1
        )[0]
        self.assertGreaterEqual(send_loop.count("manual_listening_requested_.load"), 2)
        self.assertIn("WithVoiceUploadLease", send_loop)
        self.assertIn("#if !CONFIG_PROVISIONS_GATEWAY_REQUIRED\n                auto text", application)

    def test_ready_working_watchdog_and_fast_release_are_explicit(self):
        application = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        header = (ROOT / "main/application.h").read_text(encoding="utf-8")
        self.assertIn("manual_listening_requested_.store(true,", application)
        self.assertIn("manual_listening_requested_.store(false,", application)
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
    def test_voice_upload_gate_rejects_stale_work_after_concurrent_release(self):
        test_source = textwrap.dedent(
            r"""
            #include "audio/voice_upload_gate.h"
            #include <atomic>
            #include <cassert>
            #include <thread>
            #include <vector>

            int main() {
                VoiceUploadGate gate;
                const uint32_t first_generation = gate.Open();
                assert(gate.Allows(first_generation));

                std::atomic<bool> send_started{false};
                std::atomic<bool> finish_send{false};
                std::atomic<bool> close_returned{false};
                std::atomic<int> actions_after_close{0};

                std::thread sender([&]() {
                    const bool authorized = gate.WithSendLease(first_generation, [&]() {
                        if (close_returned.load(std::memory_order_acquire)) {
                            actions_after_close.fetch_add(1, std::memory_order_relaxed);
                        }
                        send_started.store(true, std::memory_order_release);
                        while (!finish_send.load(std::memory_order_acquire)) {
                            std::this_thread::yield();
                        }
                    });
                    assert(authorized);
                });
                while (!send_started.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }

                std::thread closer([&]() {
                    gate.Close();
                    close_returned.store(true, std::memory_order_release);
                });
                for (int index = 0; index < 1000; ++index) {
                    assert(!close_returned.load(std::memory_order_acquire));
                    std::this_thread::yield();
                }
                finish_send.store(true, std::memory_order_release);
                sender.join();
                closer.join();
                assert(close_returned.load());
                assert(actions_after_close.load() == 0);
                assert(!gate.WithSendLease(first_generation, [&]() {
                    actions_after_close.fetch_add(1, std::memory_order_relaxed);
                }));

                const uint32_t second_generation = gate.Open();
                assert(second_generation != first_generation);
                assert(!gate.Allows(first_generation));
                assert(gate.Allows(second_generation));
                assert(!gate.WithSendLease(first_generation, [&]() {
                    actions_after_close.fetch_add(1, std::memory_order_relaxed);
                }));
                assert(gate.WithSendLease(second_generation, []() {}));
                assert(actions_after_close.load() == 0);
            }
            """
        )
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary = Path(temporary_directory)
            source = temporary / "voice_upload_gate_test.cc"
            executable = temporary / "voice_upload_gate_test"
            source.write_text(test_source, encoding="utf-8")
            command = [
                shutil.which("c++"),
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-pthread",
                "-I",
                str(ROOT / "main"),
                str(source),
                "-o",
                str(executable),
            ]
            subprocess.run(command, check=True, cwd=ROOT)
            subprocess.run([str(executable)], check=True, cwd=ROOT)

    @unittest.skipUnless(shutil.which("c++"), "host C++ compiler is unavailable")
    def test_endpoint_credential_and_uuid_policy(self):
        test_source = textwrap.dedent(
            r"""
            #include "provisions_endpoint_policy.h"
            #include <cassert>
#include <array>
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
                assert(!IsAllowedFirmwareUrl(
                    "https://app.provisions-app.com/kitchen-helper/preview/v1/firmware/"
                    "provisions-kitchen-helper-core-s3-lite/1.0.0/" + hash + ".bin"));
                assert(FirmwareUrlMatchesVersion(
                    "https://app.provisions-app.com/kitchen-helper/preview/v1/firmware/"
                    "provisions-kitchen-helper-core-s3/1.0.0/" + hash + ".bin", "1.0.0"));
                assert(!FirmwareUrlMatchesVersion(
                    "https://app.provisions-app.com/kitchen-helper/preview/v1/firmware/"
                    "provisions-kitchen-helper-core-s3/1.0.0/" + hash + ".bin", "1.0.1"));
                std::string extracted_version;
                assert(ExtractFirmwareVersion(
                    "https://app.provisions-app.com/kitchen-helper/preview/v1/firmware/"
                    "provisions-kitchen-helper-core-s3/1.0.0/" + hash + ".bin",
                    extracted_version));
                assert(extracted_version == "1.0.0");
                assert(IsNewerFirmwareVersion("1.0.0", "1.0.1"));
                assert(!IsNewerFirmwareVersion("1.0.0", "1.0.0"));
                assert(!IsNewerFirmwareVersion("1.0.0", "01.0.1"));
                assert(IsApprovedFirmwareImageVersion(
                    "https://app.provisions-app.com/kitchen-helper/preview/v1/firmware/"
                    "provisions-kitchen-helper-core-s3/1.0.1/" + hash + ".bin",
                    "1.0.0", "1.0.1"));
                assert(!IsApprovedFirmwareImageVersion(
                    "https://app.provisions-app.com/kitchen-helper/preview/v1/firmware/"
                    "provisions-kitchen-helper-core-s3/1.0.1/" + hash + ".bin",
                    "1.0.0", "1.0.0"));
                assert(!IsApprovedFirmwareImageVersion(
                    "https://app.provisions-app.com/kitchen-helper/preview/v1/firmware/"
                    "provisions-kitchen-helper-core-s3/1.0.1/" + hash + ".bin",
                    "1.0.1", "1.0.1"));
                std::array<uint8_t, 32> digest{};
                assert(ExtractFirmwareSha256(
                    "https://app.provisions-app.com/kitchen-helper/preview/v1/firmware/"
                    "provisions-kitchen-helper-core-s3/1.0.0/" + hash + ".bin", digest));
                for (auto byte : digest) assert(byte == 0xaa);

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
                assert(!IsAllowedFirmwareUrl(
                    "https://app.provisions-app.com/kitchen-helper/preview/v1/firmware/"
                    "provisions-kitchen-helper-core-s3/not-semver/" + hash + ".bin"));
                assert(!IsAllowedFirmwareUrl(
                    "https://app.provisions-app.com/kitchen-helper/preview/v1/firmware/"
                    "provisions-kitchen-helper-core-s3/1.70000/" + hash + ".bin"));
                assert(!IsAllowedFirmwareUrl(
                    "https://app.provisions-app.com/kitchen-helper/preview/v1/firmware/"
                    "provisions-kitchen-helper-core-s3/1.0/" + hash + ".bin"));
                assert(!IsAllowedFirmwareUrl(
                    "https://app.provisions-app.com/kitchen-helper/preview/v1/firmware/"
                    "provisions-kitchen-helper-core-s3/1.0.0.0/" + hash + ".bin"));
                assert(!IsAllowedFirmwareUrl(
                    "https://app.provisions-app.com/kitchen-helper/preview/v1/firmware/"
                    "provisions-kitchen-helper-core-s3/01.0.0/" + hash + ".bin"));

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
                f'-DBOARD_NAME="{FULL_PROFILE}"',
                "-DCONFIG_BOARD_TYPE_M5STACK_PROVISIONS_CORE_S3=1",
                "-DCONFIG_SPIRAM_MODE_QUAD=1",
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

    @unittest.skipUnless(shutil.which("c++"), "host C++ compiler is unavailable")
    def test_lite_ota_identity_accepts_only_lite_firmware(self):
        test_source = textwrap.dedent(
            r"""
            #include "provisions_endpoint_policy.h"
            #include <cassert>
            #include <string>

            int main() {
                using namespace ProvisionsEndpointPolicy;
                const std::string hash(64, 'a');
                const std::string prefix =
                    "https://app.provisions-app.com/kitchen-helper/preview/v1/firmware/";
                assert(IsAllowedFirmwareUrl(
                    prefix + "provisions-kitchen-helper-core-s3-lite/1.0.0/" +
                    hash + ".bin"));
                assert(!IsAllowedFirmwareUrl(
                    prefix + "provisions-kitchen-helper-core-s3/1.0.0/" +
                    hash + ".bin"));
            }
            """
        )
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary = Path(temporary_directory)
            source = temporary / "lite_policy_test.cc"
            executable = temporary / "lite_policy_test"
            source.write_text(test_source, encoding="utf-8")
            command = [
                shutil.which("c++"),
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Werror",
                f'-DBOARD_NAME="{LITE_PROFILE}"',
                "-DCONFIG_BOARD_TYPE_M5STACK_PROVISIONS_CORE_S3_LITE=1",
                "-DCONFIG_SPIRAM_MODE_QUAD=1",
                '-DCONFIG_PROVISIONS_PREVIEW_HOST="app.provisions-app.com"',
                '-DCONFIG_PROVISIONS_PREVIEW_PATH_PREFIX="/kitchen-helper/preview/v1/"',
                '-DCONFIG_PROVISIONS_PREVIEW_WEBSOCKET_URL="wss://app.provisions-app.com/kitchen-helper/preview/v1/device"',
                '-DCONFIG_OTA_URL="https://app.provisions-app.com/kitchen-helper/preview/v1/bootstrap"',
                "-I",
                str(ROOT / "main"),
                str(ROOT / "main/provisions_endpoint_policy.cc"),
                str(source),
                "-o",
                str(executable),
            ]
            subprocess.run(command, check=True, cwd=ROOT)
            subprocess.run([str(executable)], check=True, cwd=ROOT)

    @unittest.skipUnless(shutil.which("c++"), "host C++ compiler is unavailable")
    def test_hardware_profile_and_ota_identity_must_match_at_compile_time(self):
        test_source = "int main() { return 0; }\n"
        mismatches = (
            ("CONFIG_BOARD_TYPE_M5STACK_PROVISIONS_CORE_S3", LITE_PROFILE),
            ("CONFIG_BOARD_TYPE_M5STACK_PROVISIONS_CORE_S3_LITE", FULL_PROFILE),
            (
                "CONFIG_BOARD_TYPE_M5STACK_PROVISIONS_CORE_S3",
                "provisions-kitchen-helper-stopwatch",
            ),
            (
                "CONFIG_BOARD_TYPE_M5STACK_PROVISIONS_STOPWATCH",
                FULL_PROFILE,
            ),
        )
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary = Path(temporary_directory)
            source = temporary / "mismatched_policy_test.cc"
            source.write_text(test_source, encoding="utf-8")
            for index, (board_symbol, board_name) in enumerate(mismatches):
                executable = temporary / f"mismatched_policy_test_{index}"
                command = [
                    shutil.which("c++"),
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f'-DBOARD_NAME="{board_name}"',
                    f"-D{board_symbol}=1",
                    (
                        "-DCONFIG_SPIRAM_MODE_OCT=1"
                        if board_symbol
                        == "CONFIG_BOARD_TYPE_M5STACK_PROVISIONS_STOPWATCH"
                        else "-DCONFIG_SPIRAM_MODE_QUAD=1"
                    ),
                    '-DCONFIG_PROVISIONS_PREVIEW_HOST="app.provisions-app.com"',
                    '-DCONFIG_PROVISIONS_PREVIEW_PATH_PREFIX="/kitchen-helper/preview/v1/"',
                    '-DCONFIG_PROVISIONS_PREVIEW_WEBSOCKET_URL="wss://app.provisions-app.com/kitchen-helper/preview/v1/device"',
                    '-DCONFIG_OTA_URL="https://app.provisions-app.com/kitchen-helper/preview/v1/bootstrap"',
                    "-I",
                    str(ROOT / "main"),
                    str(ROOT / "main/provisions_endpoint_policy.cc"),
                    str(source),
                    "-o",
                    str(executable),
                ]
                result = subprocess.run(
                    command, cwd=ROOT, capture_output=True, text=True, check=False
                )
                self.assertNotEqual(result.returncode, 0)

    @unittest.skipUnless(shutil.which("c++"), "host C++ compiler is unavailable")
    def test_hardware_profile_and_psram_mode_must_match_at_compile_time(self):
        test_source = "int main() { return 0; }\n"
        invalid_profiles = (
            (
                "CONFIG_BOARD_TYPE_M5STACK_PROVISIONS_CORE_S3",
                FULL_PROFILE,
                ("CONFIG_SPIRAM_MODE_OCT",),
            ),
            (
                "CONFIG_BOARD_TYPE_M5STACK_PROVISIONS_CORE_S3_LITE",
                LITE_PROFILE,
                ("CONFIG_SPIRAM_MODE_QUAD", "CONFIG_SPIRAM_MODE_OCT"),
            ),
            (
                "CONFIG_BOARD_TYPE_M5STACK_PROVISIONS_STOPWATCH",
                "provisions-kitchen-helper-stopwatch",
                ("CONFIG_SPIRAM_MODE_QUAD",),
            ),
            (
                "CONFIG_BOARD_TYPE_M5STACK_PROVISIONS_STOPWATCH",
                "provisions-kitchen-helper-stopwatch",
                ("CONFIG_SPIRAM_MODE_OCT", "CONFIG_SPIRAM_MODE_QUAD"),
            ),
        )
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary = Path(temporary_directory)
            source = temporary / "psram_policy_test.cc"
            source.write_text(test_source, encoding="utf-8")
            for index, (board_symbol, board_name, memory_symbols) in enumerate(
                invalid_profiles
            ):
                executable = temporary / f"psram_policy_test_{index}"
                command = [
                    shutil.which("c++"),
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f'-DBOARD_NAME="{board_name}"',
                    f"-D{board_symbol}=1",
                    *(f"-D{symbol}=1" for symbol in memory_symbols),
                    '-DCONFIG_PROVISIONS_PREVIEW_HOST="app.provisions-app.com"',
                    '-DCONFIG_PROVISIONS_PREVIEW_PATH_PREFIX="/kitchen-helper/preview/v1/"',
                    '-DCONFIG_PROVISIONS_PREVIEW_WEBSOCKET_URL="wss://app.provisions-app.com/kitchen-helper/preview/v1/device"',
                    '-DCONFIG_OTA_URL="https://app.provisions-app.com/kitchen-helper/preview/v1/bootstrap"',
                    "-I",
                    str(ROOT / "main"),
                    str(ROOT / "main/provisions_endpoint_policy.cc"),
                    str(source),
                    "-o",
                    str(executable),
                ]
                result = subprocess.run(
                    command, cwd=ROOT, capture_output=True, text=True, check=False
                )
                self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
