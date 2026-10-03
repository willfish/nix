#ifndef PI_VOICE_DEVICES_H
#define PI_VOICE_DEVICES_H

typedef struct MicrophoneMonitor MicrophoneMonitor;

typedef struct MicStatus {
    char *name;
    char *target;
    int muted;
    int muted_known;
    char *preferred;
    int missing;
    char *error;
} MicStatus;

typedef struct MicRunResult {
    int ok;
    int exit_code;
    char *stdout_text;
} MicRunResult;

/* argv is the pw-dump command. timeout is seconds, at most 2. Return 0 and fill out. */
typedef int (*MicRunner)(char *const *argv, double timeout, MicRunResult *out, void *user);

MicrophoneMonitor *mic_monitor_new(const char *preferred, MicRunner runner, void *user);
void mic_monitor_free(MicrophoneMonitor *monitor);
int mic_status(MicrophoneMonitor *monitor, MicStatus *out);
int mic_resolve(MicrophoneMonitor *monitor, MicStatus *out);
void mic_status_free(MicStatus *status);
int mic_runner_pw_dump(char *const *argv, double timeout, MicRunResult *out, void *user);

#endif
