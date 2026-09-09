#!/usr/bin/env python3
"""Reproduce the off-device output audit; never open hardware or modify firmware."""
import argparse
import hashlib
import json
import math
import re
import struct
import subprocess
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[3])
parser.add_argument('--accepted-build-log', type=Path, required=True)
parser.add_argument('--verified-private-nvs-audit', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
repo = args.repo.resolve()
sha = lambda data: hashlib.sha256(data).hexdigest()
accepted = 'a84ab070dfb3205c900430902c949d899e77e4c8'
diagnostic = 'fa0b92f8ec5d0601d1aa7b6df691ef7d2004ba66'
component = repo / 'managed_components/espressif__esp_codec_dev'
manifest = json.loads((component / 'CHECKSUMS.json').read_text())
entries = {item['path']: item for item in manifest['files']}
file_names = ['esp_codec_dev.c', 'esp_codec_dev_vol.c', 'device/es8311/es8311.c',
              'include/esp_codec_dev_vol.h']
checks = []
for name in file_names:
    data = (component / name).read_bytes()
    item = entries[name]
    assert len(data) == item['size'] and sha(data) == item['hash']
    checks.append({'path': 'managed_components/espressif__esp_codec_dev/' + name,
                   'sha256': item['hash'], 'package_manifest_match': True})
assert re.search(r'^version: 1\.5\.11$', (component / 'idf_component.yml').read_text(), re.M)
build = args.accepted_build_log.read_bytes()
assert sha(build) == '0f1e22d48428f754e9011212aac11a5c0b0c506c76dd9eea58f7b3d39ee38416'
versions = [line for line in build.decode().splitlines() if line.startswith('NOTICE:')
            and any(name in line for name in ('esp_codec_dev', 'm5ioe1'))]
assert any('esp_codec_dev (1.5.11)' in line for line in versions)
assert any('m5ioe1 (1.0.9)' in line for line in versions)
source_rows = []
for commit in (accepted, diagnostic):
    values = {}
    for path in ('main/audio/codecs/es8311_audio_codec.cc',
                 'main/boards/m5stack/stopwatch/config.h',
                 'main/boards/m5stack/stopwatch/m5stack_stopwatch.cc'):
        data = subprocess.run(['git', 'show', commit + ':' + path], cwd=repo,
                              check=True, capture_output=True).stdout
        values[path] = {'sha256': sha(data)}
        source = data.decode()
        if path.endswith('es8311_audio_codec.cc'):
            assert 'es8311_cfg.hw_gain.pa_voltage = 5.0;' in source
            assert 'es8311_cfg.hw_gain.codec_dac_voltage = 3.3;' in source
            assert 'es8311_cfg.hw_gain.pa_gain' not in source
        elif path.endswith('config.h'):
            assert re.search(r'AUDIO_CODEC_GPIO_PA\s+GPIO_NUM_NC', source)
            assert re.search(r'IOE_PIN_PA_EN\s+M5IOE1_PIN_10', source)
            assert re.search(r'IOE_PIN_CODEC_POWER\s+M5IOE1_PIN_3', source)
        else:
            assert re.search(r'kDefaultOutputVolume\s*=\s*90;', source)
            assert re.search(r'kMaximumOutputVolume\s*=\s*100;', source)
            assert 'ioe_.digitalWrite(IOE_PIN_PA_EN, HIGH);' in source
            assert 'ioe_.digitalWrite(IOE_PIN_CODEC_POWER, HIGH);' in source
    source_rows.append({'commit': commit, 'files': values,
                        'shared_config_assertions_pass': True})
# Select only the non-secret volume preference. Do not output other NVS records.
audit = json.loads(args.verified_private_nvs_audit.read_text())
assert audit['summary']['official_integrity_errors'] == 0
record = [item for item in audit['private_logical_records']
          if item['namespace'] == 'audio' and item['key'] == 'output_volume']
assert len(record) == 1 and record[0]['type'] == 'int32_t' and record[0]['bytes'] == 4
matches = [n for n in range(101) if sha(struct.pack('<i', n)) == record[0]['sha256']]
assert matches == [100]
api = (component / 'esp_codec_dev.c').read_text()
set_volume = api[api.index('int esp_codec_dev_set_out_vol('):api.index('int esp_codec_dev_set_vol_handler(')]
set_mute = api[api.index('int esp_codec_dev_set_out_mute('):api.index('int esp_codec_dev_get_out_mute(')]
assert re.search(r'codec->set_vol\(codec, db_value\);\s*return ESP_CODEC_DEV_OK;', set_volume)
assert re.search(r'codec->mute\(codec, mute\);\s*return ESP_CODEC_DEV_OK;', set_mute)
gain = 20 * math.log10(3.3 / 5.0)
rows = []
for volume in (90, 100):
    db = -50.0 + 0.5 * volume
    target = db - gain
    register = int((target + 95.5) * 2)
    rows.append({'requested_percent': volume, 'default_curve_db': db,
                 'configured_hw_gain_db': gain, 'target_dac_db': target,
                 'predicted_reg32': f'0x{register:02x}',
                 'quantized_dac_db': -95.5 + register * 0.5})
result = {
    'scope': 'Off-device source and protected prior-evidence audit; no runtime modification or device action.',
    'component_version': 'esp_codec_dev 1.5.11',
    'component_files': checks,
    'accepted_build_log_sha256': sha(build),
    'accepted_build_versions': versions,
    'source_provenance': source_rows,
    'saved_volume_target': 100,
    'saved_volume_evidence': 'Only audio.output_volume int32 hash from the already-verified pre-install NVS audit was matched; raw values for all other records remain private.',
    'volume_callback_result_discarded': True,
    'mute_callback_result_discarded': True,
    'gain_model': rows,
    'gain_model_limit': 'Source-derived prediction using configured voltage assumptions and default curve. Not a hardware register readback, measured rail voltage, clipping measurement or acoustic proof.',
    'current_measurement_gap': 'No captured ES8311 register 0x31/0x32 readback and no measured amplifier rail/output. Successful software return/counter cannot close this gap.',
}
args.output.write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps({'saved_volume_target': 100, 'component_checks': len(checks),
                  'callback_acknowledgement_gap_confirmed': True,
                  'device_actions': 0, 'output': str(args.output)}))
