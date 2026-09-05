#include "anechoic/SpeechEnhancer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct WavData {
    std::vector<std::vector<int16_t>> channels;
    uint32_t sampleRate = 0;
};

uint32_t readU32(const unsigned char *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

uint16_t readU16(const unsigned char *p) {
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}

void writeU32(unsigned char *p, uint32_t v) {
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff;
    p[3] = (v >> 24) & 0xff;
}

void writeU16(unsigned char *p, uint16_t v) {
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
}

// Reads a RIFF/WAVE file with a 16-bit PCM mono or stereo stream. Iterates the
// chunk list instead of assuming a fixed layout so header chunks of arbitrary
// size are tolerated.
bool readWav(const std::string &path, WavData &wav) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) {
        std::fprintf(stderr, "error: cannot open '%s'\n", path.c_str());
        return false;
    }

    unsigned char header[12];
    if (std::fread(header, 1, sizeof(header), f) != sizeof(header) ||
        std::memcmp(header, "RIFF", 4) != 0 || std::memcmp(header + 8, "WAVE", 4) != 0) {
        std::fprintf(stderr, "error: '%s' is not a RIFF/WAVE file\n", path.c_str());
        std::fclose(f);
        return false;
    }

    bool fmtFound = false;
    bool dataFound = false;
    uint16_t numChannels = 0;
    uint16_t bitsPerSample = 0;
    uint16_t blockAlign = 0;

    while (true) {
        unsigned char chunk[8];
        if (std::fread(chunk, 1, sizeof(chunk), f) != sizeof(chunk))
            break;
        const uint32_t chunkSize = readU32(chunk + 4);
        const std::string tag(reinterpret_cast<const char *>(chunk), 4);
        const long nextPos = std::ftell(f) + static_cast<long>(chunkSize) + (chunkSize & 1);

        if (tag == "fmt ") {
            unsigned char fmt[16];
            if (chunkSize >= 16 && std::fread(fmt, 1, 16, f) == 16) {
                const uint16_t format = readU16(fmt);
                numChannels = readU16(fmt + 2);
                wav.sampleRate = readU32(fmt + 4);
                bitsPerSample = readU16(fmt + 14);
                blockAlign = readU16(fmt + 12);
                if (format != 1) {
                    std::fprintf(stderr, "error: only PCM WAV files are supported (format=%u)\n", format);
                } else if (numChannels != 1 && numChannels != 2) {
                    std::fprintf(stderr, "error: only mono/stereo WAV files are supported (channels=%u)\n",
                                 numChannels);
                } else if (bitsPerSample != 16) {
                    std::fprintf(stderr, "error: only 16-bit WAV files are supported (bits=%u)\n",
                                 bitsPerSample);
                } else {
                    fmtFound = true;
                }
            }
        } else if (tag == "data") {
            const uint32_t frameBytes = blockAlign ? blockAlign
                                                   : uint32_t(numChannels) * (bitsPerSample / 8);
            const uint64_t frameCount = frameBytes
                                                ? static_cast<uint64_t>(chunkSize) / frameBytes
                                                : 0;
            wav.channels.assign(numChannels, std::vector<int16_t>(static_cast<size_t>(frameCount)));
            const size_t samples = static_cast<size_t>(numChannels) *
                                   static_cast<size_t>(frameCount);
            if (samples > 0) {
                std::vector<int16_t> interleaved(samples);
                if (std::fread(interleaved.data(), 2, samples, f) == samples) {
                    for (uint32_t ch = 0; ch < numChannels; ++ch)
                        for (size_t i = 0; i < frameCount; ++i)
                            wav.channels[ch][i] = interleaved[i * numChannels + ch];
                    dataFound = true;
                }
            }
        }

        if (std::fseek(f, nextPos, SEEK_SET) != 0)
            break;
        if (dataFound)
            break;
    }

    std::fclose(f);

    if (!fmtFound)
        return false;
    if (!dataFound) {
        std::fprintf(stderr, "error: no valid PCM 'data' chunk found in '%s'\n", path.c_str());
        return false;
    }
    return true;
}

// Writes a mono 16-bit PCM WAVE file.
bool writeWav(const std::string &path, uint32_t sampleRate, const std::vector<float> &mono) {
    std::vector<unsigned char> pcm(mono.size() * 2);
    for (size_t i = 0; i < mono.size(); ++i) {
        float v = std::max(-1.0f, std::min(1.0f, mono[i]));
        const int32_t s = static_cast<int32_t>(std::lrint(v * 32767.0f));
        writeU16(&pcm[i * 2], static_cast<uint16_t>(s));
    }

    const uint32_t dataSize = static_cast<uint32_t>(pcm.size());

    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "error: cannot write '%s'\n", path.c_str());
        return false;
    }

    unsigned char header[44];
    std::memcpy(header, "RIFF", 4);
    writeU32(header + 4, 36 + dataSize);
    std::memcpy(header + 8, "WAVE", 4);
    std::memcpy(header + 12, "fmt ", 4);
    writeU32(header + 16, 16);
    writeU16(header + 20, 1);
    writeU16(header + 22, 1);
    writeU32(header + 24, sampleRate);
    writeU32(header + 28, sampleRate * 2);
    writeU16(header + 32, 2);
    writeU16(header + 34, 16);
    std::memcpy(header + 36, "data", 4);
    writeU32(header + 40, dataSize);

    std::fwrite(header, 1, sizeof(header), f);
    std::fwrite(pcm.data(), 1, pcm.size(), f);
    std::fclose(f);
    return true;
}

void printUsage() {
    std::fprintf(stderr,
                 "anechoic_offline - offline voice suppression using the RNNoise core\n"
                 "Usage: anechoic_offline <input.wav> <output.wav> [options]\n"
                 "Operates on 48 kHz 16-bit PCM mono/stereo WAV files. Output is written as\n"
                 "mono 16-bit PCM.\n"
                 "Options (defaults in brackets):\n"
                 "  --threshold <0..1>    VAD activation threshold (default 0.55)\n"
                 "  --grace-ms <0..500>   VAD grace period in ms (default 200)\n"
                 "  --retroactive-ms <n>  Retroactive VAD grace in ms (default 0).\n"
                 "                        Only meaningful for real-time use; keep 0 offline\n"
                 "  --hysteresis <0..1>   VAD hysteresis, subtracted while voice is active\n"
                 "                        (default 0)\n"
                 "  --comfort-db <-90..-30>  Comfort noise floor in dB instead of digital mute\n"
                 "                        (default -40)\n");
}

struct Options {
    float threshold = 0.55f;
    uint32_t graceMs = 200;
    uint32_t retroactiveMs = 0;
    float hysteresis = 0.0f;
    float comfortDb = -40.0f;
};

bool clampValue(const char *name, int &i, int argc, char **argv, float *value, float min, float max) {
    if (i + 1 >= argc) {
        std::fprintf(stderr, "error: missing value for %s\n", name);
        return false;
    }
    float v = static_cast<float>(std::atof(argv[++i]));
    *value = std::max(min, std::min(max, v));
    return true;
}

bool parseOptions(int argc, char **argv, Options &options) {
    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        float v = 0.0f;
        if (arg == "--threshold") {
            if (!clampValue("--threshold", i, argc, argv, &v, 0.0f, 1.0f))
                return false;
            options.threshold = v;
        } else if (arg == "--grace-ms") {
            if (!clampValue("--grace-ms", i, argc, argv, &v, 0.0f, 500.0f))
                return false;
            options.graceMs = static_cast<uint32_t>(std::lrint(v));
        } else if (arg == "--retroactive-ms") {
            if (!clampValue("--retroactive-ms", i, argc, argv, &v, 0.0f, 500.0f))
                return false;
            options.retroactiveMs = static_cast<uint32_t>(std::lrint(v));
        } else if (arg == "--hysteresis") {
            if (!clampValue("--hysteresis", i, argc, argv, &v, 0.0f, 1.0f))
                return false;
            options.hysteresis = v;
        } else if (arg == "--comfort-db") {
            if (!clampValue("--comfort-db", i, argc, argv, &v, -90.0f, -30.0f))
                return false;
            options.comfortDb = v;
        } else {
            std::fprintf(stderr, "error: unknown option '%s'\n", arg.c_str());
            printUsage();
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        printUsage();
        return 1;
    }

    Options options;
    if (!parseOptions(argc, argv, options))
        return 1;

    WavData wav;
    if (!readWav(argv[1], wav))
        return 1;

    if (wav.sampleRate != anechoic::kEngineRateHz) {
        std::fprintf(stderr, "error: the suppressor requires 48 kHz input, got %u Hz\n", wav.sampleRate);
        return 1;
    }

    const size_t frameCount = wav.channels[0].size();
    if (frameCount == 0) {
        std::fprintf(stderr, "error: empty input\n");
        return 1;
    }

    // Feed the file in >= 500 ms segments so every call lands on the offline
    // path of the core: it answers at once and zero-pads the tail instead of
    // building up latency waiting for more audio.
    const size_t segment = anechoic::kFrameSamples * 50;

    const uint32_t channelCount = static_cast<uint32_t>(wav.channels.size());
    anechoic::Suppressor suppressor(channelCount);

    anechoic::GateSettings settings;
    settings.threshold = options.threshold;
    settings.holdBlocks = (options.graceMs + 9u) / 10u;
    settings.rewindBlocks = (options.retroactiveMs + 9u) / 10u;
    settings.hysteresis = options.hysteresis;
    settings.muteGainDb = options.comfortDb;

    std::vector<std::vector<float>> input(channelCount, std::vector<float>(segment, 0.0f));
    std::vector<std::vector<float>> output(channelCount, std::vector<float>(segment, 0.0f));
    std::vector<const float *> inPtrs(channelCount);
    std::vector<float *> outPtrs(channelCount);
    for (uint32_t ch = 0; ch < channelCount; ++ch) {
        inPtrs[ch] = input[ch].data();
        outPtrs[ch] = output[ch].data();
    }

    std::vector<std::vector<float>> accumulated(channelCount);
    for (uint32_t ch = 0; ch < channelCount; ++ch)
        accumulated[ch].reserve(frameCount);

    size_t pos = 0;
    while (pos < frameCount) {
        const size_t segmentLen = std::min(segment, frameCount - pos);
        const size_t callLen = segment;  // full segment, zeros pad the tail

        for (uint32_t ch = 0; ch < channelCount; ++ch) {
            std::fill(input[ch].begin(), input[ch].end(), 0.0f);
            for (size_t i = 0; i < segmentLen; ++i)
                input[ch][i] = static_cast<float>(wav.channels[ch][pos + i]) / 32768.0f;
        }

        suppressor.process(inPtrs.data(), outPtrs.data(), callLen, settings);

        for (uint32_t ch = 0; ch < channelCount; ++ch)
            accumulated[ch].insert(accumulated[ch].end(), output[ch].begin(),
                                   output[ch].begin() + static_cast<std::ptrdiff_t>(segmentLen));
        pos += segmentLen;
    }

    // Downmix to mono for a stable output format.
    std::vector<float> mono(frameCount, 0.0f);
    for (size_t i = 0; i < frameCount; ++i) {
        float sum = 0.0f;
        for (uint32_t ch = 0; ch < channelCount; ++ch)
            sum += accumulated[ch][i];
        mono[i] = sum / static_cast<float>(channelCount);
    }

    if (!writeWav(argv[2], wav.sampleRate, mono))
        return 1;

    const anechoic::GateDiagnostics stats = suppressor.diagnostics();
    std::fprintf(stderr,
                 "done: %zu frames in, %zu frames out, %u channel(s)\n"
                 "gate: hold=%llu blocks rewind=%llu blocks hysteresis=%llu blocks padded=%llu samples\n",
                 frameCount, frameCount, channelCount,
                 static_cast<unsigned long long>(stats.holdBlocks),
                 static_cast<unsigned long long>(stats.rewindBlocks),
                 static_cast<unsigned long long>(stats.latchBlocks),
                 static_cast<unsigned long long>(stats.paddedSamples));
    return 0;
}