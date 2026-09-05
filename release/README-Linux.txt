Anechoic for Linux
==================

Set the microphone to 48000 Hz first.

  sudo install -Dm644 ladspa/libanechoic_ladspa.so /usr/lib/x86_64-linux-gnu/ladspa/
  ./install-preset.sh
  systemctl --user restart pipewire pipewire-pulse

Then choose "Anechoic Noise Suppression" as the microphone.

Starting values in ~/.config/pipewire/pipewire.conf.d/99-anechoic-ladspa.conf:

  "VAD Threshold (%)"        = 55
  "VAD Grace Period (ms)"    = 200
  "VAD Hysteresis (%)"       = 0
  "Comfort Noise Floor (dB)" = -40

Arch uses /usr/lib/ladspa/. Fedora uses /usr/lib64/ladspa/.
