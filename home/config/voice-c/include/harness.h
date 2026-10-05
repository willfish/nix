#ifndef PI_VOICE_HARNESS_H
#define PI_VOICE_HARNESS_H

#include "runtime_adapters.h"

#include <yyjson.h>

/* Long-running voice library facade. Does not own a controller.
 *
 * Shutdown stops playback and drains HTTP workers before unbinding microphone
 * adapters and freeing audio. Backend processes have their own lifecycle.
 */

typedef struct voice_runtime voice_runtime;

typedef struct voice_runtime_spec {
    int no_services; /* use an in-process microphone fixture */
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
voice_terminal *voice_runtime_terminal(voice_runtime *runtime);
const char *voice_runtime_path(const voice_runtime *runtime);
int voice_runtime_auto_speak(const voice_runtime *runtime);

int test_harness(void);

#endif
