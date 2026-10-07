#include "models.h"
#include <string.h>
const Asset assets[] = {
    {"models/ggml-silero-v6.2.0.bin",
     "https://huggingface.co/ggml-org/whisper-vad/resolve/"
     "9ffd54a1e1ee413ddf265af9913beaf518d1639b/ggml-silero-v6.2.0.bin",
     "2aa269b785eeb53a82983a20501ddf7c1d9c48e33ab63a41391ac6c9f7fb6987", NULL,
     UINT64_C(885098), true, false},
    {"models/ggml-large-v3-turbo-q5_0.bin",
     "https://huggingface.co/ggerganov/whisper.cpp/resolve/"
     "5359861c739e955e79d9a303bcbc70fb988958b1/ggml-large-v3-turbo-q5_0.bin",
     "394221709cd5ad1f40c46e6031ca61bce88931e6e088c188294c6d5a55ffa7e2",
     "andromeda", UINT64_C(574041195), true, false},
    {"models/supertonic-3-orig.gguf",
     "https://huggingface.co/audio-cpp/audio.cpp-gguf/resolve/"
     "dc6fecccc2b0c6bdda0a8b2f38fa61394fee0b9c/Supertonic-3-GGUF/"
     "supertonic-3-orig.gguf",
     "af814486a0bc9513fb36afabd9b1155ad14fb2c36a107ac6ffe62ea9adafb662", NULL,
     UINT64_C(454072836), false, false},
    {"models/Qwen3-TTS-12Hz-0.6B-Base-GGUF/qwen3-tts-12hz-0.6b-base-q8_0.gguf",
     "https://huggingface.co/audio-cpp/audio.cpp-gguf/resolve/"
     "dc6fecccc2b0c6bdda0a8b2f38fa61394fee0b9c/Qwen3-TTS-12Hz-0.6B-Base-GGUF/"
     "qwen3-tts-12hz-0.6b-base-q8_0.gguf",
     "771420bd20ff5f35407b4fa9cf9c5461e153800d3d772ef51c9febc0a520855d", NULL,
     UINT64_C(1991211136), false, false},
    {"models/Qwen3-TTS-12Hz-1.7B-Base-GGUF/"
     "qwen3-tts-12hz-1.7b-base-q8_0_v2.gguf",
     "https://huggingface.co/audio-cpp/audio.cpp-gguf/resolve/"
     "dc6fecccc2b0c6bdda0a8b2f38fa61394fee0b9c/Qwen3-TTS-12Hz-1.7B-Base-GGUF/"
     "qwen3-tts-12hz-1.7b-base-q8_0_v2.gguf",
     "b55e06c7890d43c208d15aed8b4ed3f18215f295e47d5960e061b15bff338ab0", NULL,
     UINT64_C(2695175104), false, true},
    {"models/dots-tts-soar-q8_0.gguf",
     "https://huggingface.co/audio-cpp/audio.cpp-gguf/resolve/"
     "dc6fecccc2b0c6bdda0a8b2f38fa61394fee0b9c/DotTTS-SOAR-GGUF/"
     "dots-tts-soar-q8_0.gguf",
     "0633a6b4b705accee3858ffb129f403dc5aa64e634a80f713f4bde6f002fc2f0", NULL,
     UINT64_C(2962749632), false, true},
    {"models/pocket-tts-english-q8_0.gguf",
     "https://huggingface.co/audio-cpp/audio.cpp-gguf/resolve/"
     "dc6fecccc2b0c6bdda0a8b2f38fa61394fee0b9c/PocketTTS-GGUF/english/"
     "pocket-tts-english-q8_0.gguf",
     "0315406421d515d9ffbde49ed998832ff2962562ef8abde440c85fa0a27d8b2a", NULL,
     UINT64_C(127856704), false, true},
    {"voices/alba.safetensors",
     "https://huggingface.co/audio-cpp/audio.cpp-gguf/resolve/"
     "dc6fecccc2b0c6bdda0a8b2f38fa61394fee0b9c/PocketTTS-GGUF/english/"
     "embeddings/alba.safetensors",
     "69c32db63ca56843d994f81f343f62e0bf2d73f7e4c9bc73e44bb1110b1d8845", NULL,
     UINT64_C(6194424), false, true},
};
const size_t assets_count = sizeof(assets) / sizeof(assets[0]);
const DownloadPolicy model_policy = {.idle_ms = 30000, .backoff_ms = 1000};
bool selected(const Asset *asset, const char *host, bool stt_only,
              bool experimental) {
  return (!stt_only || asset->stt) &&
         (!asset->host || !strcmp(asset->host, host)) &&
         (!asset->experimental || experimental);
}
