Anechoic for Windows
====================

Set the microphone to 48000 Hz first.

Equalizer APO (whole-system mic):
  1. Install Equalizer APO and enable your microphone.
  2. Add a VST plugin pointing at vst\anechoic_mono.dll
  3. Start with:

       VAD Threshold (%)          55
       VAD Grace Period (ms)     200
       Retroactive VAD Grace       0
       VAD Hysteresis (%)          0
       Comfort Noise Floor (dB)  -40

DAW:
  Copy the anechoic.vst3 folder to C:\Program Files\Common Files\VST3
