#ifndef PI_VOICE_HARNESS_H
#define PI_VOICE_HARNESS_H

#include "runtime_adapters.h"

#include <yyjson.h>

/* Long-running voice library facade. Does not own a controller.
 *
 * Shutdown order is fixed:
 * 1. audio_stop and audio_drain, so callbacks finish before engines are freed.
 * 2. unbind audio from engines.
 * 3. engine_manager_close while audio still exists, because readiness probes it.
 * 4. audio_free, then engine_manager_free.
 */

typedef struct voice_runtime voice_runtime;

typedef struct voice_runtime_spec {
    int no_services; /* no systemctl; readiness is immediately ready */
    int silent; /* playback and cue hooks do not spawn */
    const char *herdr_argv0; /* NULL selects "herdr" */
} voice_runtime_spec;

/* runtime_path NULL resolves the private pi-voice runtime directory.
 * config is an immutable owned-by-caller document and is not retained.
 */
voice_runtime *voice_runtime_open(
    const char *runtime_path, const yyjson_doc *config, const voice_runtime_spec *spec,
    char *err, size_t err_cap);
void voice_runtime_close(voice_runtime *runtime);

audio *voice_runtime_audio(voice_runtime *runtime);
engine_manager *voice_runtime_engines(voice_runtime *runtime);
voice_terminal *voice_runtime_terminal(voice_runtime *runtime);
const char *voice_runtime_path(const voice_runtime *runtime);
int voice_runtime_auto_speak(const voice_runtime *runtime);

int test_harness(void);

#endif
