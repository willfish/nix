#define _GNU_SOURCE
#include <dlfcn.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static const char *route(const char *path) {
  const char *target = getenv("NM_FIXTURE_EXEC");
  const char *name = strrchr(path, '/');
  if (target && name && !strcmp(name, "/nmcli"))
    return target;
  return path;
}
int execve(const char *path, char *const argv[], char *const envp[]) {
  static int (*real)(const char *, char *const[], char *const[]);
  if (!real) {
    void *symbol = dlsym(RTLD_NEXT, "execve");
    memcpy(&real, &symbol, sizeof real);
  }
  return real(route(path), argv, envp);
}
int execv(const char *path, char *const argv[]) {
  static int (*real)(const char *, char *const[]);
  if (!real) {
    void *symbol = dlsym(RTLD_NEXT, "execv");
    memcpy(&real, &symbol, sizeof real);
  }
  return real(route(path), argv);
}
int execvp(const char *path, char *const argv[]) {
  static int (*real)(const char *, char *const[]);
  if (!real) {
    void *symbol = dlsym(RTLD_NEXT, "execvp");
    memcpy(&real, &symbol, sizeof real);
  }
  return real(route(path), argv);
}
int posix_spawnp(pid_t *pid, const char *path,
                 const posix_spawn_file_actions_t *actions,
                 const posix_spawnattr_t *attrs, char *const argv[],
                 char *const envp[]) {
  static int (*real)(pid_t *, const char *, const posix_spawn_file_actions_t *,
                     const posix_spawnattr_t *, char *const[], char *const[]);
  if (!real) {
    void *symbol = dlsym(RTLD_NEXT, "posix_spawnp");
    memcpy(&real, &symbol, sizeof real);
  }
  return real(pid, route(path), actions, attrs, argv, envp);
}
int pidfd_spawnp(int *pid, const char *path,
                 const posix_spawn_file_actions_t *actions,
                 const posix_spawnattr_t *attrs, char *const argv[],
                 char *const envp[]) {
  static int (*real)(int *, const char *, const posix_spawn_file_actions_t *,
                     const posix_spawnattr_t *, char *const[], char *const[]);
  if (!real) {
    void *symbol = dlsym(RTLD_NEXT, "pidfd_spawnp");
    memcpy(&real, &symbol, sizeof real);
  }
  return real(pid, route(path), actions, attrs, argv, envp);
}
int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *actions,
                const posix_spawnattr_t *attrs, char *const argv[],
                char *const envp[]) {
  static int (*real)(pid_t *, const char *, const posix_spawn_file_actions_t *,
                     const posix_spawnattr_t *, char *const[], char *const[]);
  if (!real) {
    void *symbol = dlsym(RTLD_NEXT, "posix_spawn");
    memcpy(&real, &symbol, sizeof real);
  }
  return real(pid, route(path), actions, attrs, argv, envp);
}
