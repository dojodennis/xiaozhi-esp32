"""Exercise the real boot entry point without an ESP board.

NVS recovery must never discard the only key for an acknowledged local recording.
The NVS stub deliberately keeps the audio partition independent from NVS, as on
the device. Non-Provisions profiles retain their existing recovery behavior.
"""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class VoiceOutboxBootReviewTests(unittest.TestCase):
    def test_nvs_initialization_recovery_preserves_recording_key(self):
        compiler = shutil.which("c++")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            # Compile the current entry point unchanged, with only hardware and
            # application dependencies replaced. ESP_ERROR_CHECK's abort is an
            # exception so both successful boot and fail-closed boot are tested.
            (temp / "main.cc").write_text((ROOT / "main/main.cc").read_text())
            headers = {
                "esp_err.h": r'''
#pragma once
using esp_err_t = int;
constexpr int ESP_OK = 0;
constexpr int ESP_FAIL = -1;
constexpr int ESP_ERR_NVS_NO_FREE_PAGES = 10;
constexpr int ESP_ERR_NVS_NEW_VERSION_FOUND = 11;
struct BootStopped {};
#define ESP_ERROR_CHECK(result) do { if ((result) != ESP_OK) throw BootStopped{}; } while (0)
''',
                "esp_log.h": "#define ESP_LOGW(...) ((void)0)\n"
                "#define ESP_LOGE(...) ((void)0)\n"
                "#define ESP_LOGI(...) ((void)0)\n",
                "nvs.h": '#include "esp_err.h"\n',
                "nvs_flash.h": '#include "esp_err.h"\nesp_err_t nvs_flash_init();\n'
                "esp_err_t nvs_flash_erase();\n",
                "application.h": r'''
#pragma once
extern int application_starts;
class Application {
public:
    static Application& GetInstance() { static Application instance; return instance; }
    void Initialize() { ++application_starts; }
    void Run() {}
};
''',
                "driver/gpio.h": "",
                "esp_event.h": "",
                "freertos/FreeRTOS.h": "",
                "freertos/task.h": "",
            }
            for name, body in headers.items():
                path = temp / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(body)
            (temp / "review.cc").write_text(r'''
#include "esp_err.h"
#include <cassert>
extern "C" void app_main();
int application_starts = 0;
int initial_error = ESP_OK, init_calls = 0, erase_calls = 0;
bool recording_key = true;
bool audio_ciphertext = true;
esp_err_t nvs_flash_init() { return init_calls++ == 0 ? initial_error : ESP_OK; }
esp_err_t nvs_flash_erase() { ++erase_calls; recording_key = false; return ESP_OK; }
void boot(int error) {
    initial_error = error;
    init_calls = erase_calls = application_starts = 0;
    recording_key = audio_ciphertext = true;
    try { app_main(); } catch (const BootStopped&) {}
}
int main() {
    boot(ESP_OK);
    assert(erase_calls == 0 && recording_key && audio_ciphertext);
    assert(application_starts == 1);
    for (int error : {ESP_ERR_NVS_NO_FREE_PAGES, ESP_ERR_NVS_NEW_VERSION_FOUND}) {
        boot(error);
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
        assert(erase_calls == 0 && "NVS repair erased the only offline recording key");
        assert(recording_key && audio_ciphertext);
        assert(application_starts == 0 && "Do not start recording with unusable NVS");
#else
        assert(erase_calls == 1 && application_starts == 1);
#endif
    }
    boot(ESP_FAIL);
    assert(erase_calls == 0 && recording_key && audio_ciphertext);
    assert(application_starts == 0);
}
'''.replace('#include <cassert>', '#include <cassert>\n#include <initializer_list>'))
            for profile in (0, 1):
                with self.subTest(provisions_profile=profile):
                    executable = temp / f"boot-review-{profile}"
                    subprocess.run(
                        [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                         f"-DCONFIG_PROVISIONS_GATEWAY_REQUIRED={profile}",
                         "-I", str(temp), str(temp / "main.cc"),
                         str(temp / "review.cc"), "-o", str(executable)],
                        check=True, capture_output=True, text=True,
                    )
                    result = subprocess.run([str(executable)], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
