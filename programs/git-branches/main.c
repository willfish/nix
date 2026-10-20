#include <gio/gio.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define GIT(...)                                                               \
  (const char *const[]) { "git", "--no-pager", __VA_ARGS__, NULL }

typedef struct {
  char *name, *updated, *sync, *status, *subject;
  int state; /* 0: idle, 1: current, 2: another worktree */
  bool warning;
} Branch;

typedef struct {
  bool color, full;
  size_t width;
} View;

static char *safe_text(const char *text) {
  g_autofree char *valid = g_utf8_make_valid(text, -1);
  GString *safe = g_string_new(NULL);
  for (const char *p = valid; *p; p = g_utf8_next_char(p)) {
    gunichar c = g_utf8_get_char(p);
    if (g_unichar_isprint(c) && g_unichar_type(c) != G_UNICODE_FORMAT)
      g_string_append_unichar(safe, c);
    else
      g_string_append_c(safe, ' ');
  }
  return g_string_free(safe, FALSE);
}

static GBytes *run_git(const char *const *args, int *status) {
  g_autoptr(GError) error = NULL;
  g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(
      G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
  g_subprocess_launcher_setenv(launcher, "LC_ALL", "C", TRUE);
  g_autoptr(GSubprocess) process =
      g_subprocess_launcher_spawnv(launcher, args, &error);
  GBytes *out = NULL;
  g_autoptr(GBytes) err = NULL;
  if (!process ||
      !g_subprocess_communicate(process, NULL, NULL, &out, &err, &error)) {
    g_printerr("git branches: %s\n", error->message);
    exit(1);
  }
  int result = g_subprocess_get_if_exited(process)
                   ? g_subprocess_get_exit_status(process)
                   : 128 + g_subprocess_get_term_sig(process);
  if (status) {
    *status = result;
  } else if (result) {
    g_autofree char *message =
        g_bytes_get_size(err)
            ? g_strndup(g_bytes_get_data(err, NULL), g_bytes_get_size(err))
            : g_strdup("Git command failed");
    g_autofree char *safe = safe_text(g_strstrip(message));
    g_printerr("git branches: %s\n", safe);
    g_bytes_unref(out);
    exit(result);
  }
  return out;
}

static char *git_text(const char *const *args, int *status) {
  g_autoptr(GBytes) bytes = run_git(args, status);
  char *text =
      g_bytes_get_size(bytes)
          ? g_strndup(g_bytes_get_data(bytes, NULL), g_bytes_get_size(bytes))
          : g_strdup("");
  return g_strchomp(text);
}

static void branch_free(gpointer data) {
  Branch *branch = data;
  g_free(branch->name);
  g_free(branch->updated);
  g_free(branch->sync);
  g_free(branch->status);
  g_free(branch->subject);
  g_free(branch);
}

static void load_branches(GPtrArray *local, GPtrArray *remote) {
  g_autoptr(GBytes) bytes = run_git(
      GIT("for-each-ref", "--sort=-committerdate",
          "--format=%(refname)%00%(HEAD)%00%(worktreepath)%00%(upstream)%00"
          "%(upstream:track)%00%(committerdate:relative)%00%(subject)%00"
          "%(symref)%00",
          "refs/heads", "refs/remotes"),
      NULL);
  size_t length = 0;
  const char *p = g_bytes_get_data(bytes, &length);
  if (!length)
    return;
  const char *end = p + length;
  while (p < end) {
    if (*p == '\n')
      p++;
    if (p == end)
      break;
    const char *field[8];
    for (size_t i = 0; i < G_N_ELEMENTS(field); i++) {
      field[i] = p;
      const char *nul = memchr(p, '\0', (size_t)(end - p));
      if (!nul) {
        g_printerr("git branches: incomplete Git ref record\n");
        exit(1);
      }
      p = nul + 1;
    }
    if (*field[7])
      continue; /* origin/HEAD is a pointer, not another branch. */
    bool is_remote = g_str_has_prefix(field[0], "refs/remotes/");
    const char *prefix = is_remote ? "refs/remotes/" : "refs/heads/";
    if (!g_str_has_prefix(field[0], prefix)) {
      g_printerr("git branches: unexpected ref namespace\n");
      exit(1);
    }
    Branch *branch = g_new0(Branch, 1);
    branch->name = safe_text(field[0] + strlen(prefix));
    branch->state = !strcmp(field[1], "*") ? 1 : *field[2] ? 2 : 0;
    branch->updated = safe_text(field[5]);
    branch->subject = safe_text(field[6]);
    branch->warning = *field[4] != '\0';
    if (!*field[3]) {
      branch->sync = g_strdup("no upstream");
      branch->status = g_strdup("no upstream");
    } else {
      const char *upstream = field[3];
      if (g_str_has_prefix(upstream, "refs/remotes/"))
        upstream += strlen("refs/remotes/");
      else if (g_str_has_prefix(upstream, "refs/heads/"))
        upstream += strlen("refs/heads/");
      size_t n = strlen(field[4]);
      g_autofree char *track =
          n >= 2 ? g_strndup(field[4] + 1, n - 2) : g_strdup("in sync");
      branch->status =
          safe_text(!strcmp(track, "gone") ? "upstream gone" : track);
      g_autofree char *sync =
          g_strdup_printf("%s | %s", branch->status, upstream);
      branch->sync = safe_text(sync);
    }
    GPtrArray *items = is_remote ? remote : local;
    if (!is_remote && branch->state == 1)
      g_ptr_array_insert(items, 0, branch);
    else
      g_ptr_array_add(items, branch);
  }
}

static size_t char_width(gunichar c) {
  return g_unichar_iszerowidth(c) ? 0 : g_unichar_iswide(c) ? 2 : 1;
}

static size_t text_width(const char *text) {
  size_t width = 0;
  for (const char *p = text; *p; p = g_utf8_next_char(p)) {
    size_t next = char_width(g_utf8_get_char(p));
    if (width > G_MAXSIZE - next)
      g_error("display width overflow");
    width += next;
  }
  return width;
}

static void style(const View *view, const char *code) {
  if (view->color)
    printf("\033[%sm", code);
}

static void cell(const View *view, const char *text, size_t width,
                 const char *code, bool last) {
  size_t used = 0;
  bool clipped = text_width(text) > width;
  size_t limit = clipped && width >= 3 ? width - 3 : width;
  style(view, code);
  for (const char *p = text; *p; p = g_utf8_next_char(p)) {
    size_t next = char_width(g_utf8_get_char(p));
    if (next > limit - used)
      break;
    fwrite(p, 1, (size_t)(g_utf8_next_char(p) - p), stdout);
    used += next;
  }
  if (clipped && width >= 3) {
    fputs("...", stdout);
    used += 3;
  }
  if (!last)
    while (used++ < width)
      putchar(' ');
  style(view, "0");
}

static void wrapped(const View *view, const char *text, size_t indent,
                    const char *code) {
  indent = MIN(indent, view->width - 2);
  g_auto(GStrv) words = g_strsplit(text, " ", -1);
  size_t used = 0;
  style(view, code);
  for (size_t i = 0; words[i]; i++) {
    size_t word_width = text_width(words[i]);
    if (used > indent && word_width >= view->width - used) {
      putchar('\n');
      used = 0;
    } else if (used > indent) {
      putchar(' ');
      used++;
    }
    for (const char *p = words[i]; *p; p = g_utf8_next_char(p)) {
      size_t next = char_width(g_utf8_get_char(p));
      if (next > view->width - used) {
        putchar('\n');
        used = 0;
      }
      while (used < indent) {
        putchar(' ');
        used++;
      }
      fwrite(p, 1, (size_t)(g_utf8_next_char(p) - p), stdout);
      used += next;
    }
  }
  style(view, "0");
  putchar('\n');
}

static const char *state_name(const Branch *branch) {
  return branch->state == 1 ? "current" : branch->state == 2 ? "worktree" : "";
}

static void row_values(const Branch *branch, bool remote, bool full,
                       const char **row) {
  size_t i = 0;
  row[i++] = branch->name;
  if (!remote)
    row[i++] = state_name(branch);
  row[i++] = branch->updated;
  if (!remote)
    row[i++] = full ? branch->sync : branch->status;
  row[i] = branch->subject;
}

static void section(const View *view, const char *title, GPtrArray *items,
                    bool remote) {
  g_autofree char *heading = g_strdup_printf("%s (%u)", title, items->len);
  wrapped(view, heading, 0, "1");
  if (!items->len) {
    wrapped(view,
            remote ? "No remote-tracking branches." : "No local branches yet.",
            2, "0");
  } else if (view->width < 110 && !view->full) {
    for (guint i = 0; i < items->len; i++) {
      Branch *branch = g_ptr_array_index(items, i);
      g_autofree char *name =
          branch->state
              ? g_strdup_printf("%s  [%s]", branch->name, state_name(branch))
              : g_strdup(branch->name);
      g_autofree char *detail =
          remote ? g_strdup(branch->updated)
                 : g_strdup_printf("%s | %s", branch->status, branch->updated);
      wrapped(view, name, 0, branch->state == 1 ? "1;7" : "1");
      wrapped(view, detail, 2, branch->warning ? "1" : "0");
    }
  } else {
    const char *local_headers[] = {"BRANCH", "STATE", "UPDATED",
                                   view->full ? "SYNC / UPSTREAM" : "SYNC",
                                   "LAST COMMIT"};
    const char *remote_headers[] = {"BRANCH", "UPDATED", "LAST COMMIT"};
    const char **headers = remote ? remote_headers : local_headers;
    size_t columns = remote ? 3 : 5;
    size_t widths[5] = {0};
    for (size_t col = 0; col < columns; col++)
      widths[col] = text_width(headers[col]);
    for (guint i = 0; i < items->len; i++) {
      const char *row[5];
      row_values(g_ptr_array_index(items, i), remote, view->full, row);
      for (size_t col = 0; col < columns; col++)
        widths[col] = MAX(widths[col], text_width(row[col]));
    }
    if (!view->full) {
      if (remote) {
        widths[0] = MIN(widths[0], view->width / 3);
        widths[1] = MIN(widths[1], 14);
      } else {
        widths[0] = MIN(widths[0], view->width / 3);
        widths[1] = 8;
        widths[2] = MIN(widths[2], 14);
        widths[3] = MIN(widths[3], view->width / 3);
      }
      size_t used = 2 * (columns - 1);
      for (size_t col = 0; col < columns - 1; col++)
        used += widths[col];
      widths[columns - 1] = view->width - used;
    }
    for (size_t col = 0; col < columns; col++) {
      if (col)
        fputs("  ", stdout);
      cell(view, headers[col], widths[col], "1", col == columns - 1);
    }
    putchar('\n');
    for (guint i = 0; i < items->len; i++) {
      Branch *branch = g_ptr_array_index(items, i);
      const char *row[5];
      row_values(branch, remote, view->full, row);
      for (size_t col = 0; col < columns; col++) {
        const char *code = "0";
        if (col == 0)
          code = "1";
        else if (!remote && col == 1 && branch->state == 1)
          code = "7";
        else if (!remote && col == 3 && branch->warning)
          code = "1";
        if (col)
          fputs("  ", stdout);
        cell(view, row[col], widths[col], code, col == columns - 1);
      }
      putchar('\n');
    }
  }
  putchar('\n');
}

static void usage(void) {
  puts("usage: git branches [-a | -r] [--full] [--color=auto|always|never]\n"
       "\nShow branches, worktrees, upstream sync and commits.\n"
       "  -a, --all      Include remote-tracking branches\n"
       "  -r, --remotes  Show only remote-tracking branches\n"
       "  --full        Include upstream names; do not truncate columns\n"
       "  -h, --help    Show this help\n"
       "\nRead-only. Remote state is from your last fetch; never fetches.\n"
       "Updated is the tip commit's committer date, not last checkout or use.");
}

static int finish(void) {
  if (fflush(stdout) == EOF || ferror(stdout)) {
    g_printerr("git branches: could not write output\n");
    return 1;
  }
  return 0;
}

int main(int argc, char **argv) {
  bool all = false, remotes = false, full = false;
  const char *color = "auto";
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-a") || !strcmp(argv[i], "--all"))
      all = true;
    else if (!strcmp(argv[i], "-r") || !strcmp(argv[i], "--remotes"))
      remotes = true;
    else if (!strcmp(argv[i], "--full"))
      full = true;
    else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
      usage();
      return finish();
    } else if (g_str_has_prefix(argv[i], "--color="))
      color = argv[i] + strlen("--color=");
    else if (!strcmp(argv[i], "--color") && i + 1 < argc)
      color = argv[++i];
    else {
      g_printerr("git branches: unknown argument; use -h for help\n");
      return 2;
    }
  }
  if ((all && remotes) || (strcmp(color, "auto") && strcmp(color, "always") &&
                           strcmp(color, "never"))) {
    g_printerr("git branches: incompatible scope or invalid color; use -h\n");
    return 2;
  }
  View view = {.width = 100, .full = full || !isatty(STDOUT_FILENO)};
  const char *no_color = g_getenv("NO_COLOR");
  view.color =
      !strcmp(color, "always") ||
      (!strcmp(color, "auto") && isatty(STDOUT_FILENO) &&
       !(no_color && *no_color) && g_strcmp0(g_getenv("TERM"), "dumb"));
  struct winsize size;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col)
    view.width = size.ws_col;
  const char *columns = g_getenv("COLUMNS");
  guint64 parsed;
  if (columns &&
      g_ascii_string_to_unsigned(columns, 10, 1, 4096, &parsed, NULL))
    view.width = (size_t)parsed;
  /* Two cells are the minimum needed to display a wide Unicode glyph. */
  view.width = MAX(view.width, 2);
  g_autofree char *repo = git_text(GIT("rev-parse", "--git-dir"), NULL);
  g_autoptr(GPtrArray) local = g_ptr_array_new_with_free_func(branch_free);
  g_autoptr(GPtrArray) remote = g_ptr_array_new_with_free_func(branch_free);
  load_branches(local, remote);
  int head_status;
  g_autofree char *head =
      git_text(GIT("symbolic-ref", "--quiet", "--short", "HEAD"), &head_status);
  if (head_status == 1) {
    g_autofree char *commit =
        git_text(GIT("rev-parse", "--short", "HEAD"), NULL);
    g_autofree char *message =
        g_strdup_printf("HEAD detached at %s; no current branch.", commit);
    wrapped(&view, message, 0, "0");
    putchar('\n');
  } else if (head_status) {
    g_printerr("git branches: cannot read current branch\n");
    return head_status;
  } else if (!local->len ||
             ((Branch *)g_ptr_array_index(local, 0))->state != 1) {
    g_autofree char *safe = safe_text(head);
    g_autofree char *message =
        g_strdup_printf("Current branch: %s (no commits yet)", safe);
    wrapped(&view, message, 0, "0");
    putchar('\n');
  }
  if (!remotes)
    section(&view, "Local branches", local, false);
  if (all || remotes)
    section(&view, "Remote branches (last fetched)", remote, true);
  if (!remotes) {
    for (guint i = 0; i < local->len; i++) {
      if (((Branch *)g_ptr_array_index(local, i))->state == 2) {
        wrapped(&view,
                "worktree = checked out elsewhere; paths: git worktree list", 0,
                "0");
        break;
      }
    }
  }
  wrapped(&view,
          view.full
              ? "Sync uses locally stored refs; no fetch is performed."
              : "Sync vs upstream, no fetch. Try --all, --remotes or --full.",
          0, "0");
  return finish();
}
