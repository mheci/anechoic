Anechoic for macOS
==================

Set the microphone to 48000 Hz first.

  xattr -dr com.apple.quarantine "/path/to/unzipped"
  cp -R anechoic.vst3 /Library/Audio/Plug-Ins/VST3/

If an .appex is present:

  cp -R *.appex /Library/Audio/Plug-Ins/Components/

Restart the audio app and load Anechoic on the mic.

Starting values:

  VAD Threshold (%)          55
  VAD Grace Period (ms)     200
  Retroactive VAD Grace       0
  VAD Hysteresis (%)          0
  Comfort Noise Floor (dB)  -40
