#define _POSIX_C_SOURCE 200809L
#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <yyjson.h>

static int failures;
static const char *test_name;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d %s: %s\n", __FILE__, __LINE__, test_name, #cond); failures++; } } while (0)

static void test_fixture_runtime_shutdown_order(void) {
    test_name = "fixture_runtime";
    char dir[] = "/tmp/voice-runtime-XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    chmod(dir, 0700);
    const char *json = "{\"backends\":[{\"id\":\"test\",\"listen_url\":\"http://127.0.0.1:9/v1/listen\"}],\"playback_mode\":\"buffered\",\"auto_speak\":true}";
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    voice_runtime_spec spec = {.no_services = 1, .silent = 1};
    char err[256];
    voice_runtime *runtime = voice_runtime_open(dir, doc, &spec, err, sizeof err);
    CHECK(runtime != NULL);
    CHECK(voice_runtime_audio(runtime) != NULL);
    CHECK(voice_runtime_terminal(runtime) != NULL);
    CHECK(voice_runtime_auto_speak(runtime) == 1);
    CHECK(strcmp(voice_runtime_path(runtime), dir) == 0);
    audio_report report;
    CHECK(audio_status(voice_runtime_audio(runtime), &report) == 0);
    voice_runtime_close(runtime);
    yyjson_doc_free(doc);
    rmdir(dir);
}

int test_harness(void) {
    failures = 0;
    test_fixture_runtime_shutdown_order();
    return failures;
}
