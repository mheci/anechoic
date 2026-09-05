# Anechoic

Keeps your voice and drops the rest: fans, keyboards, traffic, room noise.

Works on Windows, Linux, and macOS. Set the microphone to **48000 Hz** before you start.

[Download the latest release](https://github.com/mheci/anechoic/releases)

Use the **mono** plugin for a microphone.

## Settings

These numbers are a good starting point on every platform:

```
VAD Threshold (%)          55
VAD Grace Period (ms)     200
Retroactive VAD Grace       0
VAD Hysteresis (%)          0
Comfort Noise Floor (dB)  -40
```

- Raise the threshold if noise leaks through while you are silent.
- Lower it if your voice cuts out.
- Raise hysteresis to `5`–`10` if the gate flutters.
- `-40` dB keeps a little room tone. `-90` is full mute.

## Windows

Use Equalizer APO for system-wide mic cleanup, or drop the VST3 into a DAW.

1. Unzip `win-anechoic.zip`.
2. Install [Equalizer APO](https://equalizerapo.com/) and enable your microphone in its Configurator.
3. In Equalizer APO, add a VST plugin and choose `vst/anechoic_mono.dll`.
4. Set the mic to 48000 Hz: Sound settings → Recording → your mic → Properties → Advanced.

Example `config.txt` in Equalizer APO:

```
Preamp: 0 dB
VSTPlugin: Library "C:\Program Files\Anechoic\vst\anechoic_mono.dll"
```

Then set the five knobs in the plugin window to the values above.

For a DAW, copy the whole `anechoic.vst3` folder to `C:\Program Files\Common Files\VST3` and rescan plugins.

## Linux

PipeWire is the usual path. Unzip `linux-anechoic.zip`, then:

```sh
# Debian / Ubuntu
sudo install -Dm644 ladspa/libanechoic_ladspa.so /usr/lib/x86_64-linux-gnu/ladspa/

# Arch
# sudo install -Dm644 ladspa/libanechoic_ladspa.so /usr/lib/ladspa/

# Fedora
# sudo install -Dm644 ladspa/libanechoic_ladspa.so /usr/lib64/ladspa/

./install-preset.sh
systemctl --user restart pipewire pipewire-pulse
pactl list short sources | grep -i anechoic
```

Pick **Anechoic Noise Suppression** as the microphone in your apps.

The installer writes this filter (you can edit `~/.config/pipewire/pipewire.conf.d/99-anechoic-ladspa.conf`):

```
filter.graph = {
    nodes = [
        {
            type   = ladspa
            name   = anechoic_suppressor
            plugin = ladspa/libanechoic_ladspa
            label  = noise_suppressor_mono
            control = {
                "VAD Threshold (%)"        = 55
                "VAD Grace Period (ms)"    = 200
                "VAD Hysteresis (%)"       = 0
                "Comfort Noise Floor (dB)" = -40
            }
        }
    ]
}
audio.rate = 48000
```

To undo: delete that file and restart PipeWire.

DAWs can load the LV2 or VST3 bundles from the same zip.

## macOS

1. Unzip `macos-anechoic.zip`.
2. Clear the download warning:

```sh
xattr -dr com.apple.quarantine "/path/to/unzipped"
```

3. Copy `anechoic.vst3` to `/Library/Audio/Plug-Ins/VST3` (or `~/Library/Audio/Plug-Ins/VST3`).
4. If an `.appex` is in the zip, copy it to `/Library/Audio/Plug-Ins/Components`.
5. Restart the audio app and load Anechoic on the mic channel.
6. Set the mic to 48000 Hz.

Use the same five numbers as above.

## Build

```sh
cmake -Bbuild -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build/src/common --output-on-failure
```

GPL-3.0. See [NOTICE](NOTICE) for upstream credits.
