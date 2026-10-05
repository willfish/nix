#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#ifdef __linux__
#include <pty.h>
#else
#include <util.h>
#endif
static bool same(const struct termios *a, const struct termios *b) {
  return a->c_iflag == b->c_iflag && a->c_oflag == b->c_oflag &&
         a->c_cflag == b->c_cflag && a->c_lflag == b->c_lflag &&
         !memcmp(a->c_cc, b->c_cc, NCCS) && cfgetispeed(a) == cfgetispeed(b) &&
         cfgetospeed(a) == cfgetospeed(b);
}
static bool raw_mode(const struct termios *t) {
  return !(t->c_iflag & (IGNBRK | BRKINT | IGNPAR | PARMRK | INPCK | ISTRIP |
                         INLCR | IGNCR | ICRNL | IXON | IXANY | IXOFF)) &&
         !(t->c_oflag & OPOST) && (t->c_cflag & (PARENB | CSIZE)) == CS8 &&
         !(t->c_lflag & (ECHO | ECHOE | ECHOK | ECHONL | ICANON | IEXTEN |
                         ISIG | NOFLSH | TOSTOP)) &&
         t->c_cc[VMIN] == 1 && t->c_cc[VTIME] == 0;
}
static bool send_bytes(int fd, const char *bytes, size_t len) {
  return write(fd, bytes, len) == (ssize_t)len;
}
int main(int argc, char **argv) {
  if (argc < 4)
    return 64;
  const char *mode = argv[1];
  if (!strcmp(mode, "input-error")) {
    int fd = open(".", O_RDONLY | O_DIRECTORY);
    if (fd < 0 || dup2(fd, 0) < 0)
      return 1;
    if (fd != 0)
      close(fd);
    execv(argv[2], argv + 2);
    return 127;
  }
  if (!strcmp(mode, "fd")) {
    int fd = open("/dev/null", O_RDONLY);
    if (fd < 0 || dup2(fd, 98) < 0)
      return 1;
    if (fd != 98)
      close(fd);
    execv(argv[2], argv + 2);
    return 127;
  }
  int master, slave;
  if (openpty(&master, &slave, NULL, NULL, NULL) < 0)
    return 1;
  struct termios before;
  if (tcgetattr(slave, &before) < 0)
    return 1;
  before.c_iflag |= IGNPAR | INPCK | IXOFF | IXANY;
  before.c_lflag |= ECHO | ECHOE | ECHOK | ICANON | ISIG | NOFLSH | TOSTOP;
  before.c_cc[VMIN] = 3;
  before.c_cc[VTIME] = 7;
  if (tcsetattr(slave, TCSANOW, &before) < 0 || tcgetattr(slave, &before) < 0)
    return 1;
  if (!strcmp(mode, "queued") && !send_bytes(master, "discard me", 10))
    return 1;
  pid_t pid = fork();
  if (pid < 0)
    return 1;
  if (!pid) {
    close(master);
    if (setsid() < 0 || ioctl(slave, TIOCSCTTY, 0) < 0)
      _exit(126);
    if (dup2(slave, 0) < 0 || dup2(slave, 1) < 0 || dup2(slave, 2) < 0)
      _exit(126);
    if (slave > 2)
      close(slave);
    execv(argv[2], argv + 2);
    _exit(127);
  }
  if (fcntl(master, F_SETFL, O_NONBLOCK) < 0) {
    kill(-pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return 1;
  }
  bool saw_raw = false, queued_cleared = false, canonical = true, sent = false,
       closed = false, ok = true, done = false;
  gint64 started = g_get_monotonic_time(), raw_at = 0;
  GString *output = g_string_new(NULL);
  int status = 0;
  while (g_get_monotonic_time() - started < 7000000) {
    char buffer[2048];
    ssize_t count;
    while ((count = read(master, buffer, sizeof buffer)) > 0)
      g_string_append_len(output, buffer, count);
    if (waitpid(pid, &status, WNOHANG) == pid) {
      done = true;
      break;
    }
    struct termios current;
    if (tcgetattr(slave, &current) < 0) {
      ok = false;
      break;
    }
    bool cleanup = g_str_has_prefix(mode, "cleanup");
    if (!cleanup && !(current.c_lflag & ICANON)) {
      if (!saw_raw) {
        saw_raw = raw_mode(&current);
        raw_at = g_get_monotonic_time();
        if (!saw_raw) {
          ok = false;
          break;
        }
      }
      gint64 delay = !strcmp(mode, "queued") ? 100000 : 20000;
      if (!sent && g_get_monotonic_time() - raw_at >= delay) {
        if (!strcmp(mode, "interrupt"))
          ok = kill(pid, SIGINT) == 0;
        else
          ok = send_bytes(master, !strcmp(mode, "ctrl-c") ? "\003" : "q", 1);
        if (!strcmp(mode, "queued"))
          queued_cleared = true;
        sent = true;
      }
    }
    if (cleanup) {
      canonical = canonical && (current.c_lflag & ICANON) != 0;
      if (!sent && strstr(output->str, "Type DELETE")) {
        if (!strcmp(mode, "cleanup-interrupt"))
          ok = kill(pid, SIGINT) == 0;
        else if (!strcmp(mode, "cleanup-eof"))
          ok = send_bytes(master, "\004", 1);
        else if (!strcmp(mode, "cleanup-confirm"))
          ok = send_bytes(master, "DELETE\n", 7);
        else
          ok = send_bytes(master, "\n", 1);
        sent = true;
      }
      if (sent && !closed && strstr(output->str, "Press Enter to close.")) {
        ok = send_bytes(master, "\n", 1);
        closed = true;
      }
    }
    if (!ok)
      break;
    g_usleep(1000);
  }
  if (!done) {
    kill(-pid, SIGKILL);
    waitpid(pid, &status, 0);
    ok = false;
  }
  char buffer[2048];
  ssize_t count;
  while ((count = read(master, buffer, sizeof buffer)) > 0)
    g_string_append_len(output, buffer, count);
  struct termios after;
  bool restored = tcgetattr(slave, &after) == 0 && same(&before, &after);
  int exit_code =
      WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
  char *b64 = g_base64_encode((const guchar *)output->str, output->len);
  printf("{\"status\":%d,\"restored\":%s,\"raw\":%s,\"queued_cleared\":%s,"
         "\"canonical\":%s,\"output_b64\":\"%s\"}\n",
         exit_code, restored ? "true" : "false", saw_raw ? "true" : "false",
         queued_cleared ? "true" : "false", canonical ? "true" : "false", b64);
  g_free(b64);
  g_string_free(output, true);
  close(master);
  close(slave);
  return ok ? 0 : 1;
}
