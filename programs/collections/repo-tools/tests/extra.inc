static int fixture_storage(int argc, char **argv) {
  if (argc < 3)
    return 2;
  const char *slash = strrchr(argv[2], '/');
  const char *tool = slash ? slash + 1 : argv[2];
  char **args = argv + 3;
  int nargs = argc - 3;
  if (!strcmp(tool, "nix")) {
    const char *status = getenv("STORAGE_NIX_STATUS");
    if (status && *status)
      return (int)strtol(status, NULL, 10);
  }
  const char *calls = getenv("STORAGE_CALLS");
  if (!calls)
    return 1;
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *obj = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, obj);
  yyjson_mut_obj_add_strcpy(doc, obj, "tool", tool);
  yyjson_mut_val *arr = yyjson_mut_arr(doc);
  for (int i = 0; i < nargs; i++)
    yyjson_mut_arr_add_strcpy(doc, arr, args[i]);
  yyjson_mut_obj_add_val(doc, obj, "args", arr);
  if (getenv("NIX_STORAGE_COST_FLAKE"))
    yyjson_mut_obj_add_strcpy(doc, obj, "flake", getenv("NIX_STORAGE_COST_FLAKE"));
  if (getenv("NIX_STORAGE_COST_SCOPE"))
    yyjson_mut_obj_add_strcpy(doc, obj, "scope", getenv("NIX_STORAGE_COST_SCOPE"));
  size_t len = 0;
  char *line = yyjson_mut_write(doc, 0, &len);
  yyjson_mut_doc_free(doc);
  if (!line)
    return 1;
  int fd = open(calls, O_WRONLY | O_CREAT | O_APPEND, 0600);
  if (fd < 0) {
    free(line);
    return 1;
  }
  char *full = malloc(len + 1);
  if (!full) {
    free(line);
    close(fd);
    return 1;
  }
  memcpy(full, line, len);
  full[len] = '\n';
  size_t left = len + 1;
  char *p = full;
  while (left) {
    ssize_t n = write(fd, p, left);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      free(full);
      free(line);
      close(fd);
      return 1;
    }
    p += n;
    left -= (size_t)n;
  }
  close(fd);
  free(full);
  free(line);
  if (!strcmp(tool, "nh")) {
    const char *status = getenv("NH_FIXTURE_STATUS");
    return status && *status ? (int)strtol(status, NULL, 10) : 0;
  }
  const char *data_text = getenv("STORAGE_DATA");
  yyjson_doc *data = yyjson_read(data_text ? data_text : "{}", data_text ? strlen(data_text) : 2, 0);
  yyjson_val *data_root = data ? yyjson_doc_get_root(data) : NULL;
  if (!strcmp(tool, "nix-store")) {
    if (nargs && !strcmp(args[0], "--query")) {
      const char *last = args[nargs - 1];
      yyjson_doc_free(data);
      if (last && strstr(last, "missing"))
        return 1;
      puts("hash");
      return 0;
    }
    const char *last = nargs ? args[nargs - 1] : "";
    yyjson_val *closures = data_root ? yyjson_obj_get(data_root, "closures") : NULL;
    yyjson_val *list = closures ? yyjson_obj_get(closures, last) : NULL;
    if (list && yyjson_is_arr(list)) {
      size_t n = yyjson_arr_size(list);
      for (size_t i = 0; i < n; i++) {
        if (i)
          putchar('\n');
        fputs(yyjson_get_str(yyjson_arr_get(list, i)), stdout);
      }
      putchar('\n');
    } else
      puts(last);
    yyjson_doc_free(data);
    return 0;
  }
  if (!strcmp(tool, "nix") && nargs && !strcmp(args[0], "build")) {
    bool home = false;
    for (int i = 0; i < nargs; i++)
      if (strstr(args[i], "homeConfigurations"))
        home = true;
    puts(home ? "/nix/store/fixture-home" : "/nix/store/fixture-system");
    yyjson_doc_free(data);
    return 0;
  }
  if (!strcmp(tool, "nix") && nargs && !strcmp(args[0], "eval")) {
    bool json = false;
    for (int i = 0; i < nargs; i++)
      if (!strcmp(args[i], "--json"))
        json = true;
    if (json) {
      const char *inventory = getenv("STORAGE_INVENTORY");
      puts(inventory ? inventory : "[]");
    } else {
      const char *home = getenv("STORAGE_SELECTED_HOME");
      puts(home ? home : "william-linux");
    }
    yyjson_doc_free(data);
    return 0;
  }
  if (!strcmp(tool, "nix") && nargs && !strcmp(args[0], "path-info")) {
    int fmt = -1;
    for (int i = 0; i < nargs; i++)
      if (!strcmp(args[i], "--json-format"))
        fmt = i;
    if (fmt < 0 || fmt + 1 >= nargs || strcmp(args[fmt + 1], "1")) {
      yyjson_doc_free(data);
      return 22;
    }
    yyjson_mut_doc *out = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(out);
    yyjson_mut_doc_set_root(out, root);
    yyjson_val *sizes = data_root ? yyjson_obj_get(data_root, "sizes") : NULL;
    for (int i = 0; i < nargs; i++) {
      if (strncmp(args[i], "/nix/store/", 11))
        continue;
      yyjson_val *size = sizes ? yyjson_obj_get(sizes, args[i]) : NULL;
      double nar = 5;
      if (size && yyjson_is_num(size))
        nar = yyjson_get_num(size);
      yyjson_mut_val *item = yyjson_mut_obj(out);
      if (nar == (double)(int)nar)
        yyjson_mut_obj_add_int(out, item, "narSize", (int)nar);
      else
        yyjson_mut_obj_add_real(out, item, "narSize", nar);
      yyjson_mut_obj_add_int(out, item, "closureSize", 7);
      yyjson_mut_obj_remove_key(root, args[i]);
      yyjson_mut_obj_add_val(out, root, args[i], item);
    }
    size_t n = 0;
    char *text = yyjson_mut_write(out, 0, &n);
    if (text) {
      fwrite(text, 1, n, stdout);
      putchar('\n');
      free(text);
    }
    yyjson_mut_doc_free(out);
    yyjson_doc_free(data);
    return text ? 0 : 1;
  }
  yyjson_doc_free(data);
  return 23;
}

static char *audit_base;
static int audit_serial;
static void audit_init(void) {
  if (!audit_base)
    audit_base = temp_dir("skill-audit-");
}
static void write_text(const char *path, const char *text) {
  write_file(path, text, strlen(text), 0600);
}
typedef struct {
  char *root, *llm, *catalog;
} AuditFix;
static AuditFix audit_fixture(void) {
  audit_init();
  AuditFix f = {0};
  char *name = g_strdup_printf("%d", audit_serial++);
  f.root = g_build_filename(audit_base, name, NULL);
  g_free(name);
  f.llm = g_build_filename(f.root, "home/config/llm", NULL);
  f.catalog = g_build_filename(f.llm, "skill-catalog.json", NULL);
  write_text(g_build_filename(f.llm, "skills/example/SKILL.md", NULL),
             "---\nname: example\ndescription: A fixture skill\n---\nRead references/example.md\n");
  write_text(g_build_filename(f.llm, "guides/example.md", NULL), "Reference\n");
  write_text(f.catalog,
             "[{\"name\":\"example\",\"kind\":\"shared\",\"invocation\":\"reference-backed domain skill\",\"hermes\":false,\"references\":{\"example.md\":\"guides/example.md\"}}]");
  return f;
}
static Proc audit_run(const char *root, bool parity) {
  char *bin = sibling("audit-skills");
  char *argv[] = {bin, "--root", (char *)root, NULL};
  Proc p = proc_run(bin, argv, NULL, NULL, 10000);
  g_free(bin);
  if (p.failed || p.code < 0 || (p.err && strstr(p.err, "Sanitizer"))) {
    fprintf(stderr, "audit spawn/sanitizer: %s\n", p.err ? p.err : "");
    p.failed = true;
  }
  const char *reference = getenv("SKILL_AUDIT_REFERENCE");
  if (parity && reference && *reference && !p.failed) {
    char *argv_old[] = {(char *)reference, "--root", (char *)root, NULL};
    Proc old = proc_run(reference, argv_old, NULL, NULL, 10000);
    if (old.code != p.code ||
        (!(p.err && g_str_has_prefix(p.err, "Invalid skill catalogue:")) &&
         ((p.out ? p.out : "") != (old.out ? old.out : "") &&
          strcmp(p.out ? p.out : "", old.out ? old.out : "")))) {
      fprintf(stderr, "audit parity\n%s\n%s\n%s\n%s\n", p.out ? p.out : "", p.err ? p.err : "",
              old.out ? old.out : "", old.err ? old.err : "");
      p.failed = true;
    }
    proc_free(&old);
  }
  return p;
}
static void audit_reject(const char *json) {
  AuditFix f = audit_fixture();
  write_text(f.catalog, json);
  Proc p = audit_run(f.root, true);
  if (p.failed || p.code != 1 || !p.err || !g_str_has_prefix(p.err, "Invalid skill catalogue:") ||
      p.out_len != 0) {
    fprintf(stderr, "reject failed: %s\n%s\n", json, p.err ? p.err : "");
    fail(__LINE__, "reject");
  }
  proc_free(&p);
  g_free(f.root);
  g_free(f.llm);
  g_free(f.catalog);
}
static void test_audit_valid(void) {
  test_name = "valid catalogue";
  AuditFix f = audit_fixture();
  Proc p = audit_run(f.root, true);
  CHECK(!p.failed && p.code == 0);
  proc_free(&p);
  char *repo = repo_root();
  CHECK(repo);
  p = audit_run(repo, true);
  CHECK(!p.failed && p.code == 0);
  proc_free(&p);
  g_free(repo);
  g_free(f.root);
  g_free(f.llm);
  g_free(f.catalog);
}
static void test_audit_schema(void) {
  test_name = "catalogue schema";
  const char *values[] = {"{}", "[]", "null", "[null]", "[\"example\"]"};
  for (size_t i = 0; i < 5; i++) {
    audit_reject(values[i]);
    if (failures)
      return;
  }
  const char *bad[] = {
      "[{\"name\":\"../example\",\"kind\":\"shared\",\"invocation\":\"reference-backed domain skill\",\"hermes\":false,\"references\":{\"example.md\":\"guides/example.md\"}}]",
      "[{\"name\":\"Example\",\"kind\":\"shared\",\"invocation\":\"reference-backed domain skill\",\"hermes\":false,\"references\":{\"example.md\":\"guides/example.md\"}}]",
      "[{\"name\":\"a\",\"kind\":\"shared\",\"invocation\":\"reference-backed domain skill\",\"hermes\":false,\"references\":{\"example.md\":\"guides/example.md\"}}]",
      "[{\"name\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"kind\":\"shared\",\"invocation\":\"reference-backed domain skill\",\"hermes\":false,\"references\":{\"example.md\":\"guides/example.md\"}}]",
      "[{\"name\":\"two--hyphens\",\"kind\":\"shared\",\"invocation\":\"reference-backed domain skill\",\"hermes\":false,\"references\":{\"example.md\":\"guides/example.md\"}}]",
      "[{\"name\":null,\"kind\":\"shared\",\"invocation\":\"reference-backed domain skill\",\"hermes\":false,\"references\":{\"example.md\":\"guides/example.md\"}}]",
      "[{\"name\":\"example\",\"kind\":\"unknown\",\"invocation\":\"reference-backed domain skill\",\"hermes\":false,\"references\":{\"example.md\":\"guides/example.md\"}}]",
      "[{\"name\":\"example\",\"kind\":[],\"invocation\":\"reference-backed domain skill\",\"hermes\":false,\"references\":{\"example.md\":\"guides/example.md\"}}]",
      "[{\"name\":\"example\",\"kind\":\"shared\",\"invocation\":\"automatic\",\"hermes\":false,\"references\":{\"example.md\":\"guides/example.md\"}}]",
      "[{\"name\":\"example\",\"kind\":\"shared\",\"invocation\":{},\"hermes\":false,\"references\":{\"example.md\":\"guides/example.md\"}}]",
      "[{\"name\":\"example\",\"kind\":\"shared\",\"invocation\":\"reference-backed domain skill\",\"hermes\":\"false\",\"references\":{\"example.md\":\"guides/example.md\"}}]",
      "[{\"name\":\"example\",\"kind\":\"shared\",\"invocation\":\"reference-backed domain skill\",\"hermes\":1,\"references\":{\"example.md\":\"guides/example.md\"}}]",
      "[{\"name\":\"example\",\"kind\":\"shared\",\"invocation\":\"reference-backed domain skill\",\"hermes\":false,\"references\":[]}]",
      "[{\"kind\":\"shared\",\"invocation\":\"reference-backed domain skill\",\"hermes\":false,\"references\":{\"example.md\":\"guides/example.md\"}}]",
      "[{\"name\":\"example\",\"invocation\":\"reference-backed domain skill\",\"hermes\":false,\"references\":{\"example.md\":\"guides/example.md\"}}]",
      "[{\"name\":\"example\",\"kind\":\"shared\",\"hermes\":false,\"references\":{\"example.md\":\"guides/example.md\"}}]",
      "[{\"name\":\"example\",\"kind\":\"shared\",\"invocation\":\"reference-backed domain skill\",\"references\":{\"example.md\":\"guides/example.md\"}}]",
      "[{\"name\":\"example\",\"kind\":\"shared\",\"invocation\":\"reference-backed domain skill\",\"hermes\":false}]",
      "[{\"name\":\"example\",\"kind\":\"shared\",\"invocation\":\"reference-backed domain skill\",\"hermes\":false,\"references\":{\"example.md\":\"guides/example.md\"},\"unexpected\":true}]"};
  for (size_t i = 0; i < G_N_ELEMENTS(bad); i++) {
    audit_reject(bad[i]);
    if (failures)
      return;
  }
}
static void test_audit_process(void) {
  test_name = "process entries";
  AuditFix f = audit_fixture();
  char *skills = g_build_filename(f.llm, "skills", NULL);
  char *process = g_build_filename(f.llm, "process-skills", NULL);
  CHECK(rename(skills, process) == 0);
  write_text(g_build_filename(process, "example/SKILL.md", NULL),
             "---\ndescription: Process fixture\n---\n");
  write_text(f.catalog,
             "[{\"name\":\"example\",\"kind\":\"process\",\"invocation\":\"reference-backed domain skill\",\"hermes\":false,\"references\":{}}]");
  Proc p = audit_run(f.root, true);
  CHECK(!p.failed && p.code == 0);
  proc_free(&p);
  const char *bad[] = {
      "[{\"name\":\"example\",\"kind\":\"process\",\"invocation\":\"reference-backed domain skill\",\"hermes\":true,\"references\":{}}]",
      "[{\"name\":\"example\",\"kind\":\"process\",\"invocation\":\"reference-backed domain skill\",\"hermes\":false,\"references\":{\"example.md\":\"guides/example.md\"}}]"};
  for (size_t i = 0; i < 2; i++) {
    write_text(f.catalog, bad[i]);
    p = audit_run(f.root, true);
    CHECK(!p.failed && p.code == 1);
    proc_free(&p);
  }
  g_free(skills);
  g_free(process);
  g_free(f.root);
  g_free(f.llm);
  g_free(f.catalog);
}
static void test_audit_duplicate(void) {
  test_name = "duplicate skills";
  AuditFix f = audit_fixture();
  const char *texts[] = {
      "[{\"name\":\"example\",\"kind\":\"shared\",\"invocation\":\"reference-backed domain skill\",\"hermes\":false,\"references\":{\"example.md\":\"guides/example.md\"}},{\"name\":\"example\",\"kind\":\"shared\",\"invocation\":\"reference-backed domain skill\",\"hermes\":false,\"references\":{\"example.md\":\"guides/example.md\"}}]",
      "[{\"name\":\"example\",\"name\":\"other\"}]",
      "[{\"references\":{\"same.md\":\"a\",\"same.md\":\"b\"}}]"};
  for (size_t i = 0; i < 3; i++) {
    write_text(f.catalog, texts[i]);
    Proc p = audit_run(f.root, true);
    CHECK(!p.failed && p.code == 1);
    CHECK(p.err && strstr(p.err, "duplicate"));
    CHECK(p.out_len == 0);
    proc_free(&p);
  }
  g_free(f.root);
  g_free(f.llm);
  g_free(f.catalog);
}
static void test_audit_paths(void) {
  test_name = "mapped paths";
  AuditFix f = audit_fixture();
  const char *sources[] = {"../outside.md", "/etc/passwd", "guides/../guides/example.md",
                           "guides//example.md", "./guides/example.md", "guides\\example.md",
                           "guides/missing.md", "guides", "", NULL};
  for (size_t i = 0; i < 10; i++) {
    char *json = sources[i]
                     ? g_strdup_printf(
                           "[{\"name\":\"example\",\"kind\":\"shared\",\"invocation\":\"reference-backed domain skill\",\"hermes\":false,\"references\":{\"example.md\":%s%s%s}}]",
                           strcmp(sources[i], "") ? "\"" : "", sources[i],
                           strcmp(sources[i], "") ? "\"" : "\"\"")
                     : g_strdup(
                           "[{\"name\":\"example\",\"kind\":\"shared\",\"invocation\":\"reference-backed domain skill\",\"hermes\":false,\"references\":{\"example.md\":null}}]");
    if (sources[i] && !strcmp(sources[i], ""))
      json = g_strdup(
          "[{\"name\":\"example\",\"kind\":\"shared\",\"invocation\":\"reference-backed domain skill\",\"hermes\":false,\"references\":{\"example.md\":\"\"}}]");
    write_text(f.catalog, json);
    Proc p = audit_run(f.root, true);
    CHECK(!p.failed && p.code == 1);
    proc_free(&p);
    g_free(json);
  }
  const char *targets[] = {"../escape.md", "/escape.md", "nested/escape.md", "bad\\name.md",
                           "script.sh", ".md"};
  for (size_t i = 0; i < 6; i++) {
    char *json = g_strdup_printf(
        "[{\"name\":\"example\",\"kind\":\"shared\",\"invocation\":\"reference-backed domain skill\",\"hermes\":false,\"references\":{\"%s\":\"guides/example.md\"}}]",
        targets[i]);
    write_text(f.catalog, json);
    Proc p = audit_run(f.root, true);
    CHECK(!p.failed && p.code == 1);
    proc_free(&p);
    g_free(json);
  }
  char *outside = g_build_filename(audit_base, "outside.md", NULL);
  write_text(outside, "synthetic private content");
  char *escape = g_build_filename(f.llm, "guides/escape.md", NULL);
  CHECK(symlink(outside, escape) == 0);
  write_text(f.catalog,
             "[{\"name\":\"example\",\"kind\":\"shared\",\"invocation\":\"reference-backed domain skill\",\"hermes\":false,\"references\":{\"example.md\":\"guides/escape.md\"}}]");
  Proc p = audit_run(f.root, true);
  CHECK(p.err && strstr(p.err, "escapes source root"));
  CHECK(!p.out || !strstr(p.out, "synthetic"));
  proc_free(&p);
  g_free(outside);
  g_free(escape);
  g_free(f.root);
  g_free(f.llm);
  g_free(f.catalog);
}
static void test_audit_collision(void) {
  test_name = "local collisions";
  AuditFix f = audit_fixture();
  write_text(g_build_filename(f.llm, "skills/example/references/example.md", NULL), "local\n");
  Proc p = audit_run(f.root, true);
  CHECK(p.err && strstr(p.err, "duplicates local reference"));
  proc_free(&p);
  unlink(g_build_filename(f.llm, "skills/example/references/example.md", NULL));
  unlink(g_build_filename(f.llm, "skills/example/SKILL.md", NULL));
  p = audit_run(f.root, true);
  CHECK(p.err && strstr(p.err, "missing catalogue source"));
  proc_free(&p);
  g_free(f.root);
  g_free(f.llm);
  g_free(f.catalog);
}
static void test_audit_frontmatter(void) {
  test_name = "frontmatter descriptions";
  AuditFix f = audit_fixture();
  const char *descriptions[] = {"description: \"Café test\"", "description: >-\n  Café\n\tsecond line",
                                "description: |\n  \n  one line", "description: \"one\"\ndescription: two"};
  for (size_t i = 0; i < 4; i++) {
    char *text = g_strdup_printf("---\n%s\n---\nreferences/example.md\n", descriptions[i]);
    write_text(g_build_filename(f.llm, "skills/example/SKILL.md", NULL), text);
    g_free(text);
    Proc p = audit_run(f.root, true);
    CHECK(!p.failed && p.code == 0);
    proc_free(&p);
  }
  write_text(g_build_filename(f.llm, "skills/example/SKILL.md", NULL), "description: not frontmatter\n");
  Proc p = audit_run(f.root, true);
  CHECK(!p.failed && p.code == 1);
  proc_free(&p);
  g_free(f.root);
  g_free(f.llm);
  g_free(f.catalog);
}
static void test_audit_findings(void) {
  test_name = "unregistered disk skills";
  AuditFix f = audit_fixture();
  write_text(g_build_filename(f.llm, "skills/extra/SKILL.md", NULL),
             "---\ndescription: extra fixture\n---\nreferences/missing.md\n");
  write_text(g_build_filename(f.llm, "skills/example/references/unmentioned.md", NULL), "local\n");
  Proc p = audit_run(f.root, true);
  CHECK(!p.failed && p.code == 1);
  const char *need[] = {"missing invocation classification", "absent from sharedSkillNames",
                        "not local or mapped", "not mentioned by SKILL.md"};
  for (size_t i = 0; i < 4; i++)
    CHECK(p.out && strstr(p.out, need[i]));
  proc_free(&p);
  g_free(f.root);
  g_free(f.llm);
  g_free(f.catalog);
}
static void test_audit_manual(void) {
  test_name = "manual-only metadata";
  AuditFix f = audit_fixture();
  write_text(f.catalog,
             "[{\"name\":\"example\",\"kind\":\"shared\",\"invocation\":\"user-invoked router/orchestrator\",\"hermes\":false,\"references\":{\"example.md\":\"guides/example.md\"}}]");
  Proc p = audit_run(f.root, true);
  CHECK(!p.failed && p.code == 1);
  CHECK(p.out && strstr(p.out, "no agents/openai.yaml"));
  CHECK(strstr(p.out, "missing Pi disable-model-invocation"));
  proc_free(&p);
  write_text(g_build_filename(f.llm, "skills/example/agents/openai.yaml", NULL),
             "policy:\n  allow_implicit_invocation: false\n");
  write_text(g_build_filename(f.llm, "skills/example/SKILL.md", NULL),
             "---\ndescription: Fixture\ndisable-model-invocation: YES\n---\nreferences/example.md\n");
  p = audit_run(f.root, true);
  CHECK(!p.failed && p.code == 0);
  proc_free(&p);
  write_text(f.catalog,
             "[{\"name\":\"example\",\"kind\":\"shared\",\"invocation\":\"user-invoked router/orchestrator\",\"hermes\":false,\"references\":{\"example.md\":\"guides/example.md\"}},{\"name\":\"superpowers\",\"kind\":\"process\",\"invocation\":\"process guardrail\",\"hermes\":false,\"references\":{}}]");
  write_text(g_build_filename(f.llm, "process-skills/superpowers/SKILL.md", NULL),
             "---\ndescription: Guardrail\ndisable-model-invocation: true\n---\n");
  p = audit_run(f.root, true);
  CHECK(p.out && strstr(p.out, "should stay model-invoked"));
  proc_free(&p);
  g_free(f.root);
  g_free(f.llm);
  g_free(f.catalog);
}
static void test_audit_long(void) {
  test_name = "long-reference contents";
  AuditFix f = audit_fixture();
  char *path = g_build_filename(f.llm, "guides/long.md", NULL);
  GString *body = g_string_new(NULL);
  for (int i = 0; i < 101; i++)
    g_string_append(body, "line\n");
  write_file(path, body->str, body->len, 0600);
  Proc p = audit_run(f.root, true);
  CHECK(!p.failed && p.code == 1);
  proc_free(&p);
  g_string_prepend(body, "# Table of Contents\n");
  write_file(path, body->str, body->len, 0600);
  p = audit_run(f.root, true);
  CHECK(!p.failed && p.code == 0);
  proc_free(&p);
  g_string_truncate(body, 0);
  for (int i = 0; i < 40; i++)
    g_string_append(body, "line\n");
  g_string_append(body, "## Contents\n");
  for (int i = 0; i < 70; i++)
    g_string_append(body, "line\n");
  write_file(path, body->str, body->len, 0600);
  p = audit_run(f.root, true);
  CHECK(!p.failed && p.code == 1);
  proc_free(&p);
  g_string_free(body, TRUE);
  g_free(path);
  g_free(f.root);
  g_free(f.llm);
  g_free(f.catalog);
}
static void test_audit_overlap(void) {
  test_name = "overlap terms";
  AuditFix f = audit_fixture();
  GString *entries = g_string_new("[");
  for (int i = 0; i < 5; i++) {
    if (i)
      g_string_append_c(entries, ',');
    g_string_append_printf(
        entries,
        "{\"name\":\"fixture-%d\",\"kind\":\"shared\",\"invocation\":\"reference-backed domain skill\",\"hermes\":false,\"references\":{}}",
        i);
    char *rel = g_strdup_printf("skills/fixture-%d/SKILL.md", i);
    char *path = g_build_filename(f.llm, rel, NULL);
    char *text = g_strdup_printf("---\ndescription: Review review verification skills %s\n---\n",
                                 i < 4 ? "updates" : "");
    write_text(path, text);
    g_free(text);
    g_free(path);
    g_free(rel);
  }
  g_string_append_c(entries, ']');
  char *example = g_build_filename(f.llm, "skills/example", NULL);
  char *argv[] = {"rm", "-rf", "--", example, NULL};
  g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
  write_text(f.catalog, entries->str);
  Proc p = audit_run(f.root, true);
  CHECK(!p.failed && p.code == 0);
  CHECK(p.out && strstr(p.out, "review                   fixture-0"));
  CHECK(strstr(p.out, "updates                  fixture-0"));
  proc_free(&p);
  g_string_free(entries, TRUE);
  g_free(example);
  g_free(f.root);
  g_free(f.llm);
  g_free(f.catalog);
}
static void test_audit_freshness(void) {
  test_name = "freshness markers";
  AuditFix f = audit_fixture();
  char *path = g_build_filename(f.llm, "guides/dates.md", NULL);
  const char *dates[] = {"2000-01-01", "20000101", "2020-W53-7", "2020W537", "2020-W53",
                         "2020-W54-1", "2023-02-29", "not-a-date"};
  for (size_t i = 0; i < 8; i++) {
    char *text = g_strdup_printf("Source: fixture\nChecked: %s\nUpdate trigger: change\n", dates[i]);
    write_text(path, text);
    g_free(text);
    Proc p = audit_run(f.root, true);
    CHECK(!p.failed && p.code == 1);
    CHECK(p.out && strstr(p.out, "Source=1 Checked=1 Update trigger=1"));
    proc_free(&p);
  }
  write_text(path, "Source: fixture\n");
  Proc p = audit_run(f.root, true);
  CHECK(p.out && strstr(p.out, "Source header missing Checked:"));
  proc_free(&p);
  write_text(path, "Checked: 9999-01-01\n");
  p = audit_run(f.root, true);
  CHECK(!p.failed && p.code == 0);
  proc_free(&p);
  g_free(path);
  g_free(f.root);
  g_free(f.llm);
  g_free(f.catalog);
}
static void test_audit_symlink(void) {
  test_name = "symlinked LLM root";
  AuditFix f = audit_fixture();
  char *moved = g_build_filename(audit_base, "llm-alias-target", NULL);
  CHECK(rename(f.llm, moved) == 0);
  CHECK(symlink(moved, f.llm) == 0);
  Proc p = audit_run(f.root, false);
  CHECK(!p.failed && p.code == 0);
  CHECK(p.out && strstr(p.out, "No findings."));
  proc_free(&p);
  g_free(moved);
  g_free(f.root);
  g_free(f.llm);
  g_free(f.catalog);
}
static void test_audit_cli(void) {
  test_name = "CLI help";
  char *bin = sibling("audit-skills");
  char *help[] = {bin, "--help", NULL};
  Proc p = proc_run(bin, help, NULL, NULL, 10000);
  CHECK(p.code == 0);
  proc_free(&p);
  char *unknown[] = {bin, "--unknown", NULL};
  p = proc_run(bin, unknown, NULL, NULL, 10000);
  CHECK(p.code == 2);
  proc_free(&p);
  AuditFix f = audit_fixture();
  char *sentinel = g_build_filename(f.root, "must-not-exist", NULL);
  write_text(g_build_filename(f.root, "flake.nix", NULL), "builtins.throw \"not evaluated\"");
  char *script = g_strdup_printf("#!/bin/sh\ntouch '%s'\n", sentinel);
  write_file(g_build_filename(f.llm, "scripts/audit-skills", NULL), script, strlen(script), 0700);
  g_free(script);
  p = audit_run(f.root, false);
  CHECK(!p.failed && p.code == 0);
  CHECK(!g_file_test(sentinel, G_FILE_TEST_EXISTS));
  proc_free(&p);
  char *missing = g_build_filename(audit_base, "missing", NULL);
  char *argv[] = {bin, "--root", missing, NULL};
  p = proc_run(bin, argv, NULL, NULL, 10000);
  CHECK(p.code == 1);
  proc_free(&p);
  g_free(missing);
  g_free(sentinel);
  g_free(bin);
  g_free(f.root);
  g_free(f.llm);
  g_free(f.catalog);
}

typedef struct {
  char *root, *bin, *flake, *home, *script, *calls;
  char **env;
} Store;
static char **env_without_nh(void) {
  char **out = g_new0(char *, 1);
  for (char **e = environ; e && *e; e++) {
    if (!strncmp(*e, "NH_", 3))
      continue;
    size_t n = 0;
    while (out[n])
      n++;
    char **next = g_realloc(out, (n + 2) * sizeof *next);
    if (!next)
      abort();
    out = next;
    out[n] = g_strdup(*e);
    out[n + 1] = NULL;
  }
  return out;
}
static void tool_script(const char *dir, const char *name, const char *body) {
  char *path = g_build_filename(dir, name, NULL);
  char *text = g_strconcat("#!/bin/sh\nset -eu\n", body, "\n", NULL);
  write_file(path, text, strlen(text), 0755);
  g_free(text);
  g_free(path);
}
static Store store_fixture(void) {
  Store f = {0};
  f.root = temp_dir("storage-report-");
  f.bin = g_build_filename(f.root, "bin", NULL);
  f.flake = g_build_filename(f.root, "selected flake", NULL);
  f.home = g_build_filename(f.root, "home", NULL);
  g_mkdir_with_parents(f.bin, 0700);
  g_mkdir_with_parents(f.flake, 0700);
  f.script = g_build_filename(f.home, ".dotfiles/scripts/nix-storage-costs", NULL);
  char *repo = repo_root();
  char *source = repo ? g_build_filename(repo, "scripts/nix-storage-costs", NULL) : NULL;
  size_t n = 0;
  char *body = source ? read_file(source, &n) : NULL;
  if (body)
    write_file(f.script, body, n, 0755);
  g_free(body);
  g_free(source);
  g_free(repo);
  f.calls = g_build_filename(f.root, "calls.jsonl", NULL);
  f.env = env_without_nh();
  char *state = g_build_filename(f.root, "state", NULL);
  const char *old = g_getenv("PATH");
  char *path = old ? g_strconcat(f.bin, ":", old, NULL) : g_strdup(f.bin);
  env_set(&f.env, "HOME", f.home);
  env_set(&f.env, "XDG_STATE_HOME", state);
  env_set(&f.env, "PATH", path);
  env_set(&f.env, "STORAGE_CALLS", f.calls);
  g_free(path);
  g_free(state);
  tool_script(f.bin, "uname", "printf \"%s\\n\" \"${STORAGE_PLATFORM:-Linux}\"");
  tool_script(f.bin, "hostname", "printf \"%s\\n\" \"${STORAGE_HOST:-andromeda}\"");
  tool_script(f.bin, "whoami", "printf \"william\\n\"");
  char *exe = self_exe();
  char *wrapper = g_strdup_printf("#!/bin/sh\nexec '%s' --fixture-storage \"$0\" \"$@\"\n", exe);
  for (int i = 0; i < 3; i++) {
    const char *name = (const char *[]){"nix", "nix-store", "nh"}[i];
    char *path = g_build_filename(f.bin, name, NULL);
    write_file(path, wrapper, strlen(wrapper), 0755);
    g_free(path);
  }
  char *report = sibling("nix-storage-report");
  char *link = g_build_filename(f.bin, "nix-storage-report", NULL);
  if (symlink(report, link) != 0) {
    fail(__LINE__, "create reporter fixture link");
    exit(1);
  }
  g_free(link);
  g_free(report);
  g_free(wrapper);
  g_free(exe);
  return f;
}
static Proc store_run(Store *f, const char **args, int nargs) {
  GPtrArray *av = g_ptr_array_new();
  g_ptr_array_add(av, (gpointer) "bash");
  g_ptr_array_add(av, f->script);
  for (int i = 0; i < nargs; i++)
    g_ptr_array_add(av, (gpointer)args[i]);
  g_ptr_array_add(av, NULL);
  Proc p = proc_run("bash", (char **)av->pdata, f->root, f->env, 15000);
  g_ptr_array_free(av, FALSE);
  return p;
}
static GPtrArray *read_calls(const char *path, const char *tool) {
  GPtrArray *out = g_ptr_array_new();
  size_t n = 0;
  char *text = read_file(path, &n);
  if (!text)
    return out;
  size_t i = 0;
  while (i < n) {
    size_t start = i;
    while (i < n && text[i] != '\n')
      i++;
    if (i > start) {
      yyjson_doc *doc = yyjson_read(text + start, i - start, 0);
      if (doc && val_streq(yyjson_obj_get(yyjson_doc_get_root(doc), "tool"), tool))
        g_ptr_array_add(out, doc);
      else
        yyjson_doc_free(doc);
    }
    if (i < n)
      i++;
  }
  g_free(text);
  return out;
}
static bool one_build(Store *f, const char *fragment) {
  GPtrArray *calls = read_calls(f->calls, "nix");
  int builds = 0;
  yyjson_val *args = NULL;
  for (guint i = 0; i < calls->len; i++) {
    yyjson_val *a = yyjson_obj_get(yyjson_doc_get_root(calls->pdata[i]), "args");
    if (val_streq(yyjson_arr_get(a, 0), "build")) {
      builds++;
      args = a;
    }
  }
  bool found = false;
  if (args)
    for (size_t i = 0; i < yyjson_arr_size(args); i++)
      if (val_streq(yyjson_arr_get(args, i), fragment))
        found = true;
  for (guint i = 0; i < calls->len; i++)
    yyjson_doc_free(calls->pdata[i]);
  g_ptr_array_free(calls, TRUE);
  if (builds != 1 || !found)
    fprintf(stderr, "build fragment missing: %s\n", fragment);
  return builds == 1 && found;
}
static void test_storage_scope(void) {
  test_name = "home and system scopes";
  Store f = store_fixture();
  const char *args[] = {"--scope", "home", "--flake", f.flake, "--home", "william-darwin"};
  Proc p = store_run(&f, args, 6);
  CHECK(p.code == 0);
  char *fragment = g_strconcat(f.flake, "#homeConfigurations.\"william-darwin\".activationPackage", NULL);
  CHECK(one_build(&f, fragment));
  char *md = g_build_filename(f.root, "state/nix-storage-costs/andromeda-william-darwin-storage-costs.md", NULL);
  size_t n = 0;
  char *text = read_file(md, &n);
  CHECK(text && !mem_has(text, n, "NixOS system:"));
  g_free(text);
  proc_free(&p);
  g_free(fragment);
  g_free(md);
  f = store_fixture();
  const char *sys[] = {"--scope", "system", "--flake", f.flake, "--host", "foundation"};
  p = store_run(&f, sys, 6);
  CHECK(p.code == 0);
  fragment = g_strconcat(f.flake, "#nixosConfigurations.\"foundation\".config.system.build.toplevel", NULL);
  CHECK(one_build(&f, fragment));
  proc_free(&p);
  g_free(fragment);
}
static void test_storage_defaults(void) {
  test_name = "Darwin and Linux defaults";
  Store f = store_fixture();
  env_set(&f.env, "STORAGE_PLATFORM", "Darwin");
  env_set(&f.env, "STORAGE_HOST", "relay");
  env_set(&f.env, "STORAGE_SELECTED_HOME", "william-darwin");
  const char *args[] = {"--flake", f.flake};
  Proc p = store_run(&f, args, 2);
  CHECK(p.code == 0);
  char *fragment = g_strconcat(f.flake, "#homeConfigurations.\"william-darwin\".activationPackage", NULL);
  CHECK(one_build(&f, fragment));
  proc_free(&p);
  g_free(fragment);
  f = store_fixture();
  const char *linux_args[] = {"--flake", f.flake, "--home", "william-linux"};
  p = store_run(&f, linux_args, 4);
  CHECK(p.code == 0);
  GPtrArray *calls = read_calls(f.calls, "nix");
  int builds = 0;
  bool flakes = true;
  for (guint i = 0; i < calls->len; i++) {
    yyjson_val *root = yyjson_doc_get_root(calls->pdata[i]);
    yyjson_val *a = yyjson_obj_get(root, "args");
    if (val_streq(yyjson_arr_get(a, 0), "build")) {
      builds++;
      if (!val_streq(yyjson_obj_get(root, "flake"), f.flake))
        flakes = false;
    }
    yyjson_doc_free(calls->pdata[i]);
  }
  g_ptr_array_free(calls, TRUE);
  CHECK(builds == 2 && flakes);
  proc_free(&p);
}
static void test_storage_nh(void) {
  test_name = "nh flags";
  Store f = store_fixture();
  const char *args[] = {"--after-nh-switch", "home", "switch", "--diff", "always",
                        "--configuration=william-darwin", f.flake, "--cores", "2"};
  Proc p = store_run(&f, args, 9);
  CHECK(p.code == 0);
  char *fragment = g_strconcat(f.flake, "#homeConfigurations.\"william-darwin\".activationPackage", NULL);
  CHECK(one_build(&f, fragment));
  proc_free(&p);
  g_free(fragment);
  f = store_fixture();
  env_set(&f.env, "NH_OS_FLAKE", f.flake);
  const char *os[] = {"--after-nh-switch", "os", "switch", "--hostname=foundation"};
  p = store_run(&f, os, 4);
  CHECK(p.code == 0);
  fragment = g_strconcat(f.flake, "#nixosConfigurations.\"foundation\".config.system.build.toplevel", NULL);
  CHECK(one_build(&f, fragment));
  proc_free(&p);
  g_free(fragment);
  f = store_fixture();
  env_set(&f.env, "NH_HOME_FLAKE", f.flake);
  env_set(&f.env, "NH_FLAKE", "/wrong/flake");
  const char *home[] = {"--after-nh-switch", "home", "switch", "-c", "william-linux"};
  p = store_run(&f, home, 5);
  CHECK(p.code == 0);
  fragment = g_strconcat(f.flake, "#homeConfigurations.\"william-linux\".activationPackage", NULL);
  CHECK(one_build(&f, fragment));
  proc_free(&p);
  g_free(fragment);
}
static bool no_nix(Store *f) {
  GPtrArray *calls = read_calls(f->calls, "nix");
  bool empty = calls->len == 0;
  for (guint i = 0; i < calls->len; i++)
    yyjson_doc_free(calls->pdata[i]);
  g_ptr_array_free(calls, TRUE);
  return empty;
}
static void test_storage_dry(void) {
  test_name = "dry and help never call Nix";
  const char *sets[][3] = {{"--dry", NULL, NULL}, {"-n", NULL, NULL}, {"--help", NULL, NULL},
                           {"-h", NULL, NULL}, {"--file", "/unrelated/home.nix", NULL},
                           {"-c", "william-linux", NULL}};
  int counts[] = {1, 1, 1, 1, 2, 2};
  for (int i = 0; i < 6; i++) {
    Store f = store_fixture();
    const char *args[8];
    int n = 0;
    args[n++] = "--after-nh-switch";
    args[n++] = "home";
    args[n++] = "switch";
    if (i < 4)
      args[n++] = f.flake;
    for (int j = 0; j < counts[i]; j++)
      args[n++] = sets[i][j];
    Proc p = store_run(&f, args, n);
    CHECK(p.code == 0);
    CHECK(no_nix(&f));
    proc_free(&p);
  }
}
static void test_storage_skip(void) {
  test_name = "resolved store outputs";
  Store f = store_fixture();
  char *output = g_build_filename(f.root, "realized-output", NULL);
  char *link = g_build_filename(f.root, "result", NULL);
  g_mkdir_with_parents(output, 0700);
  CHECK(symlink(output, link) == 0);
  tool_script(f.bin, "realpath", "printf \"/nix/store/fixture-system\\n\"");
  const char *args[] = {"--after-nh-switch", "os", "switch", link};
  Proc p = store_run(&f, args, 4);
  CHECK(p.code == 0);
  CHECK(p.err && strstr(p.err, "Storage-cost callback skipped"));
  CHECK(no_nix(&f));
  proc_free(&p);
  const char *extras[][4] = {{"--override-input", "nixpkgs", "/other", NULL},
                             {"--", "--override-input", "name", "/other"}};
  int ns[] = {3, 4};
  for (int i = 0; i < 2; i++) {
    f = store_fixture();
    const char *run[12] = {"--after-nh-switch", "home", "switch", f.flake, "-c", "william-linux"};
    int n = 6;
    for (int j = 0; j < ns[i]; j++)
      run[n++] = extras[i][j];
    p = store_run(&f, run, n);
    CHECK(p.code == 0);
    CHECK(no_nix(&f));
    proc_free(&p);
  }
  g_free(output);
  g_free(link);
}
static void test_storage_separator(void) {
  test_name = "empty Nix argument separator";
  Store f = store_fixture();
  const char *args[] = {"--after-nh-switch", "home", "switch", f.flake, "-c", "william-linux", "--"};
  Proc p = store_run(&f, args, 7);
  CHECK(p.code == 0);
  char *fragment = g_strconcat(f.flake, "#homeConfigurations.\"william-linux\".activationPackage", NULL);
  CHECK(one_build(&f, fragment));
  proc_free(&p);
  g_free(fragment);
}
static char *between_first(const char *src, const char *start, const char *end, int *count) {
  int n = 0;
  char *first = NULL;
  const char *p = src;
  size_t sl = strlen(start), el = strlen(end);
  while ((p = strstr(p, start))) {
    const char *body = p + sl;
    const char *stop = strstr(body, end);
    if (!stop)
      break;
    if (!first)
      first = g_strndup(body, (size_t)(stop - body));
    n++;
    p = stop + el;
  }
  if (count)
    *count = n;
  return first;
}
static void test_storage_inventory(void) {
  test_name = "inventory expression";
  char *repo = repo_root();
  CHECK(repo);
  char *path = g_build_filename(repo, "scripts/nix-storage-costs", NULL);
  size_t n = 0;
  char *source = read_file(path, &n);
  CHECK(source);
  GPtrArray *exprs = g_ptr_array_new_with_free_func(g_free);
  const char *p = source;
  while ((p = strstr(p, "--expr '"))) {
    p += 8;
    const char *end = strchr(p, '\'');
    CHECK(end);
    g_ptr_array_add(exprs, g_strndup(p, (size_t)(end - p)));
    p = end + 1;
  }
  CHECK(exprs->len == 2);
  char *inventory = replace_first(exprs->pdata[1],
                                  "builtins.getFlake (builtins.getEnv \"NIX_STORAGE_COST_FLAKE\")",
                                  "fixture");
  CHECK(inventory);
  const char *library =
      "rec { optionals = condition: values: if condition then values else []; concatStringsSep = builtins.concatStringsSep; attrByPath = path: fallback: attrs: if path == [] then attrs else if builtins.hasAttr (builtins.head path) attrs then attrByPath (builtins.tail path) fallback attrs.${builtins.head path} else fallback; }";
  const char *home =
      "{ fixture.config = { home.packages = []; programs = { git.package = null; fish.package = null; zoxide.package = null; direnv.package = null; }; }; }";
  const char *system =
      "{ fixture.config = { environment = { systemPackages = []; shells = []; }; users.users.william.packages = []; users.defaultUserShell = null; programs.fish.package = null; virtualisation.docker.package = null; boot.kernelPackages.kernel = null; fonts.packages = []; }; }";
  const char *scopes[] = {"home", "system"};
  for (int i = 0; i < 2; i++) {
    char *expression = g_strdup_printf(
        "let fixture = { inputs.nixpkgs.lib = %s; homeConfigurations = %s; nixosConfigurations = %s; }; in %s",
        library, i == 0 ? home : "throw \"unselected home\"",
        i == 1 ? system : "throw \"unselected system\"", inventory);
    char *argv[] = {"nix-instantiate", "--eval", "--strict", "--json", "--expr", expression, NULL};
    char **env = env_copy(environ);
    env_set(&env, "NIX_STORAGE_COST_SCOPE", scopes[i]);
    env_set(&env, "NIX_STORAGE_COST_HOST", "fixture");
    env_set(&env, "NIX_STORAGE_COST_HOME", "fixture");
    Proc p = proc_run("nix-instantiate", argv, NULL, env, 15000);
    CHECK(p.code == 0);
    size_t start = 0, end = p.out_len;
    while (start < end && (p.out[start] == ' ' || p.out[start] == '\n'))
      start++;
    while (end > start && (p.out[end - 1] == ' ' || p.out[end - 1] == '\n'))
      end--;
    CHECK(end - start == 2 && !memcmp(p.out + start, "[]", 2));
    proc_free(&p);
    env_free(env);
    g_free(expression);
  }
  g_free(inventory);
  g_ptr_array_free(exprs, TRUE);
  g_free(source);
  g_free(path);
  g_free(repo);
}
static char *shell_code(const char *shell) {
  char *repo = repo_root();
  char *path = g_build_filename(repo, "home/user/shells.nix", NULL);
  size_t n = 0;
  char *source = read_file(path, &n);
  g_free(path);
  g_free(repo);
  if (!source)
    return NULL;
  if (!strcmp(shell, "bash")) {
    int count = 0;
    char *body = between_first(source, "initExtra = ''\n", "\n    '';", &count);
    g_free(source);
    if (count != 1) {
      g_free(body);
      return NULL;
    }
    char *out = replace_first(body, "''${", "${");
    if (!out)
      out = g_strdup(body);
    else {
      char *cursor = out;
      for (;;) {
        char *next = replace_first(cursor, "''${", "${");
        if (!next)
          break;
        if (cursor != out)
          g_free(cursor);
        cursor = next;
      }
      if (cursor != out) {
        g_free(out);
        out = cursor;
      }
    }
    g_free(body);
    return out;
  }
  GString *joined = g_string_new(NULL);
  const char *names[] = {"__storage_costs_after_nh_switch", "nh"};
  for (int i = 0; i < 2; i++) {
    char *start = g_strdup_printf("      %s = ''\n", names[i]);
    int count = 0;
    char *body = between_first(source, start, "\n      '';", &count);
    g_free(start);
    if (count != 1) {
      g_free(body);
      g_string_free(joined, TRUE);
      g_free(source);
      return NULL;
    }
    g_string_append_printf(joined, "function %s\n%s\nend\n", names[i], body);
    g_free(body);
  }
  g_free(source);
  return g_string_free(joined, FALSE);
}
static Proc shell_run(Store *f, const char *shell, const char **args, int nargs) {
  unlink(f->calls);
  char *code = shell_code(shell);
  char *path = g_strdup_printf("%s/source.%s", f->root, shell);
  if (!code || !write_file(path, code, strlen(code), 0600)) {
    g_free(code);
    g_free(path);
    Proc bad = {0};
    bad.failed = true;
    bad.code = -1;
    return bad;
  }
  g_free(code);
  GPtrArray *av = g_ptr_array_new();
  g_ptr_array_add(av, (gpointer)shell);
  if (!strcmp(shell, "bash")) {
    g_ptr_array_add(av, (gpointer) "--noprofile");
    g_ptr_array_add(av, (gpointer) "--norc");
    g_ptr_array_add(av, (gpointer) "-c");
    g_ptr_array_add(av, (gpointer) "source \"$1\"; shift; nh \"$@\"");
    g_ptr_array_add(av, (gpointer) "fixture");
  } else {
    g_ptr_array_add(av, (gpointer) "--no-config");
    g_ptr_array_add(av, (gpointer) "-c");
    g_ptr_array_add(av, (gpointer) "source $argv[1]; nh $argv[2..-1]");
  }
  g_ptr_array_add(av, path);
  for (int i = 0; i < nargs; i++)
    g_ptr_array_add(av, (gpointer)args[i]);
  g_ptr_array_add(av, NULL);
  Proc p = proc_run(shell, (char **)av->pdata, f->root, f->env, 15000);
  g_ptr_array_free(av, FALSE);
  g_free(path);
  return p;
}
static void test_storage_shell(void) {
  test_name = "Bash and Fish";
  Store f = store_fixture();
  const char *args[] = {"home", "switch", "--diff", "always", f.flake, "-c", "william-darwin"};
  const char *shells[] = {"bash", "fish"};
  for (int i = 0; i < 2; i++) {
    Proc p = shell_run(&f, shells[i], args, 7);
    CHECK(p.code == 0);
    proc_free(&p);
    GPtrArray *nh = read_calls(f.calls, "nh");
    CHECK(nh->len >= 1);
    yyjson_val *got = yyjson_obj_get(yyjson_doc_get_root(nh->pdata[0]), "args");
    CHECK(yyjson_arr_size(got) == 7);
    for (int j = 0; j < 7; j++)
      CHECK(val_streq(yyjson_arr_get(got, (size_t)j), args[j]));
    for (guint k = 0; k < nh->len; k++)
      yyjson_doc_free(nh->pdata[k]);
    g_ptr_array_free(nh, TRUE);
    char *fragment = g_strconcat(f.flake, "#homeConfigurations.\"william-darwin\".activationPackage", NULL);
    CHECK(one_build(&f, fragment));
    g_free(fragment);
  }
  env_set(&f.env, "NH_FIXTURE_STATUS", "7");
  for (int i = 0; i < 2; i++) {
    const char *fail_args[] = {"home", "switch", f.flake};
    Proc p = shell_run(&f, shells[i], fail_args, 3);
    CHECK(p.code == 7);
    CHECK(no_nix(&f));
    proc_free(&p);
  }
  env_set(&f.env, "NH_FIXTURE_STATUS", "0");
  env_set(&f.env, "STORAGE_NIX_STATUS", "19");
  for (int i = 0; i < 2; i++) {
    Proc p = shell_run(&f, shells[i], args, 7);
    CHECK(p.code == 0);
    proc_free(&p);
  }
}
static bool has_text(const char *path, const char *needle) {
  size_t n = 0;
  char *text = read_file(path, &n);
  bool ok = text && mem_has(text, n, needle);
  g_free(text);
  return ok;
}
static void test_storage_accounting(void) {
  test_name = "shared closure accounting";
  Store f = store_fixture();
  const char *inventory =
      "[{\"storePath\":\"/nix/store/A\",\"name\":\"Alpha\",\"pname\":\"Alpha\",\"source\":\"nixos\",\"label\":\"one\"},"
      "{\"storePath\":\"/nix/store/A\",\"name\":\"Alpha\",\"pname\":\"Alpha\",\"source\":\"home-manager\",\"label\":\"two\"},"
      "{\"storePath\":\"/nix/store/B\",\"name\":\"Beta\",\"pname\":\"Beta\",\"source\":\"nixos\",\"label\":\"three\"},"
      "{\"storePath\":\"/nix/store/C\",\"name\":\"Alpha\",\"pname\":\"Alpha\",\"source\":\"home-manager\",\"label\":\"four\"},"
      "{\"storePath\":\"/nix/store/missing\",\"name\":\"Unrealized\",\"pname\":\"Unrealized\",\"source\":\"home-manager\",\"label\":\"five\"}]";
  const char *data =
      "{\"closures\":{\"/nix/store/A\":[\"/nix/store/A\",\"/nix/store/shared\",\"/nix/store/X\"],\"/nix/store/B\":[\"/nix/store/B\",\"/nix/store/shared\"],\"/nix/store/C\":[\"/nix/store/C\",\"/nix/store/shared\",\"/nix/store/Z\"]},\"sizes\":{\"/nix/store/A\":100,\"/nix/store/B\":200,\"/nix/store/C\":50,\"/nix/store/shared\":1000,\"/nix/store/X\":300,\"/nix/store/Z\":700}}";
  env_set(&f.env, "STORAGE_INVENTORY", inventory);
  env_set(&f.env, "STORAGE_DATA", data);
  const char *args[] = {"--scope", "home", "--flake", f.flake, "--home", "william@fixture", "--summary",
                        "--color", "never"};
  Proc p = store_run(&f, args, 9);
  CHECK(p.code == 0);
  CHECK(p.out && strstr(p.out, "inventory_roots=3"));
  CHECK(strstr(p.out, "skipped_unrealized_roots=1"));
  CHECK(strstr(p.out, "inventory_closure=2.3 KiB"));
  CHECK(p.err && strstr(p.err, "skipping 1 unrealized"));
  char *prefix = g_build_filename(f.root, "state/nix-storage-costs/andromeda-william_fixture-storage-costs", NULL);
  char *csv = g_strconcat(prefix, ".csv", NULL);
  char *md = g_strconcat(prefix, ".md", NULL);
  CHECK(has_text(csv, "Alpha,Alpha,home-manager,four,/nix/store/C,50,1750,750,3,2\r\n"));
  CHECK(has_text(csv, "Alpha,Alpha,\"home-manager,nixos\",\"one,two\",/nix/store/A,100,1400,400,3,2\r\n"));
  CHECK(has_text(md, "| `Alpha` | 2 | `home-manager,nixos` | 1.1 KiB | 2.1 KiB | 150 B |"));
  CHECK(!mem_has(p.out, p.out_len, "\x1b"));
  proc_free(&p);
  const char *color[] = {"--scope", "home", "--flake", f.flake, "--home", "william@fixture", "--summary",
                         "--color", "always"};
  p = store_run(&f, color, 9);
  CHECK(p.code == 0);
  CHECK(mem_has(p.out, p.out_len, "\x1b[1;36m"));
  proc_free(&p);
  g_free(csv);
  g_free(md);
  g_free(prefix);
}
