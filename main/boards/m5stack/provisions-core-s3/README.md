# Provisions Kitchen Helper CoreS3

Gate 1 board profile for the full M5Stack CoreS3. It keeps the stock CoreS3
microphone, speaker, display, PMIC, battery reporting and charging support. It
does not initialize touch, camera or a wake word.

Connect a M5Stack Unit Button U027 to Port.B. Its active-low signal is GPIO8.
Hold the button to talk and release it to stop the microphone turn.

The build is locked to the preview namespace at
`app.provisions-app.com/kitchen-helper/preview/v1`. It accepts the device bearer
only from the untracked `provisions.device_token` NVS value. Do not put a token,
Wi-Fi SSID or Wi-Fi password in this repository.

Build with ESP-IDF 6.0.2:

```sh
python3 scripts/build.py m5stack/provisions-core-s3 \
  --name provisions-kitchen-helper-core-s3 \
  --language en-US \
  --wake-word disabled
```

Factory provisioning must erase any stock Xiaozhi image first, flash the
firmware, then write a separately generated per-device NVS image containing the
known demo Wi-Fi, board UUID and device token. Keep that image outside source
control.

This profile still requires a physical CoreS3/U027 smoke test for button
polarity, audio, battery, charging, display, reconnects and short Talk presses.
