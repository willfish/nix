#define _POSIX_C_SOURCE 200809L
#include "bridge.h"
#include <NetworkManager.h>
#include <fcntl.h>
#include <nm-secret-agent-old.h>
#include <signal.h>
#include <string.h>

struct Bridge {
  NMSecretAgentOld *agent;
  void *state;
  const Callbacks *callbacks;
  guint tick;
  struct sigaction term, interrupt;
};
typedef struct {
  NMSecretAgentOld parent;
  Bridge *bridge;
} DotAgent;
typedef struct {
  NMSecretAgentOldClass parent;
} DotAgentClass;
G_DEFINE_TYPE(DotAgent, dot_agent, NM_TYPE_SECRET_AGENT_OLD)
static volatile sig_atomic_t caught_signal;
static void on_signal(int number) { caught_signal = number; }
int bridge_signal(void) { return caught_signal; }
int bridge_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL);
  return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
int bridge_printable(uint32_t ch) {
  if (ch == ' ')
    return 1;
  switch (g_unichar_type(ch)) {
  case G_UNICODE_CONTROL:
  case G_UNICODE_FORMAT:
  case G_UNICODE_UNASSIGNED:
  case G_UNICODE_PRIVATE_USE:
  case G_UNICODE_SURROGATE:
  case G_UNICODE_SPACE_SEPARATOR:
  case G_UNICODE_LINE_SEPARATOR:
  case G_UNICODE_PARAGRAPH_SEPARATOR:
    return 0;
  default:
    return 1;
  }
}
static void get_secrets(NMSecretAgentOld *agent, NMConnection *connection,
                        const char *path, const char *setting_name,
                        const char **hints, NMSecretAgentGetSecretsFlags flags,
                        NMSecretAgentOldGetSecretsFunc callback,
                        gpointer user_data) {
  (void)hints;
  Bridge *bridge = ((DotAgent *)agent)->bridge;
  char *secret = NULL, *message = NULL;
  bridge->callbacks->get(bridge->state, nm_connection_get_id(connection),
                         nm_connection_get_uuid(connection), path, setting_name,
                         flags, &secret, &message);
  if (caught_signal) {
    // Destruction completes the pending libnm request as AgentCanceled. Do not
    // invoke its now-invalid completion callback after cancellation.
    nm_secret_agent_old_destroy(agent);
  } else if (message) {
    GError *error = g_error_new_literal(
        NM_SECRET_AGENT_ERROR, NM_SECRET_AGENT_ERROR_NO_SECRETS, message);
    callback(agent, connection, NULL, error, user_data);
    g_error_free(error);
  } else {
    NMConnection *secrets = nm_simple_connection_new();
    NMSetting *setting = nm_setting_wireless_security_new();
    g_object_set(setting, NM_SETTING_WIRELESS_SECURITY_PSK, secret, NULL);
    nm_connection_add_setting(secrets, setting);
    GVariant *dict = g_variant_ref_sink(
        nm_connection_to_dbus(secrets, NM_CONNECTION_SERIALIZE_ALL));
    callback(agent, connection, dict, NULL, user_data);
    g_variant_unref(dict);
    g_object_unref(secrets);
  }
  bridge->callbacks->release(secret);
  bridge->callbacks->release(message);
}
static void cancel(NMSecretAgentOld *agent, const char *path,
                   const char *setting) {
  Bridge *b = ((DotAgent *)agent)->bridge;
  b->callbacks->cancel(b->state, path, setting);
}
static void save(NMSecretAgentOld *agent, NMConnection *connection,
                 const char *path, NMSecretAgentOldSaveSecretsFunc callback,
                 gpointer data) {
  (void)path;
  callback(agent, connection, NULL, data);
}
static void delete(NMSecretAgentOld *agent, NMConnection *connection,
                   const char *path, NMSecretAgentOldDeleteSecretsFunc callback,
                   gpointer data) {
  (void)path;
  callback(agent, connection, NULL, data);
}
static void dot_agent_class_init(DotAgentClass *klass) {
  NMSecretAgentOldClass *parent = NM_SECRET_AGENT_OLD_CLASS(klass);
  parent->get_secrets = get_secrets;
  parent->cancel_get_secrets = cancel;
  parent->save_secrets = save;
  parent->delete_secrets = delete;
}
static void dot_agent_init(DotAgent *agent) { (void)agent; }
static gboolean tick(gpointer data) {
  (void)data;
  return G_SOURCE_CONTINUE;
}
Bridge *bridge_new(void *state, const Callbacks *callbacks, char **message) {
  Bridge *b = g_new0(Bridge, 1);
  b->state = state;
  b->callbacks = callbacks;
  b->agent = g_object_new(dot_agent_get_type(), NM_SECRET_AGENT_OLD_IDENTIFIER,
                          "org.dotfiles.nm-auto-secret-agent",
                          NM_SECRET_AGENT_OLD_AUTO_REGISTER, FALSE,
                          NM_SECRET_AGENT_OLD_CAPABILITIES, 0, NULL);
  ((DotAgent *)b->agent)->bridge = b;
  GError *error = NULL;
  if (!g_initable_init(G_INITABLE(b->agent), NULL, &error)) {
    *message = g_strdup(error->message);
    g_clear_error(&error);
    g_object_unref(b->agent);
    g_free(b);
    return NULL;
  }
  caught_signal = 0;
  struct sigaction action = {0};
  action.sa_handler = on_signal;
  sigemptyset(&action.sa_mask);
  sigaction(SIGTERM, &action, &b->term);
  sigaction(SIGINT, &action, &b->interrupt);
  b->tick = g_timeout_add(20, tick, NULL);
  return b;
}
static void registered(GObject *object, GAsyncResult *result, gpointer data) {
  Bridge *b = data;
  GError *error = NULL;
  gboolean ok = nm_secret_agent_old_register_finish(NM_SECRET_AGENT_OLD(object),
                                                    result, &error);
  b->callbacks->registered(
      b->state, ok ? nm_secret_agent_old_get_registered(b->agent) : 0,
      error ? error->message : NULL);
  g_clear_error(&error);
}
void bridge_start(Bridge *b) {
  nm_secret_agent_old_enable(b->agent, TRUE);
  nm_secret_agent_old_register_async(b->agent, NULL, registered, b);
}
void bridge_step(void) { g_main_context_iteration(NULL, TRUE); }
void bridge_free_error(char *message) { g_free(message); }
void bridge_destroy(Bridge *b) {
  GDBusConnection *connection =
      g_object_ref(nm_secret_agent_old_get_dbus_connection(b->agent));
  nm_secret_agent_old_destroy(b->agent);
  // libnm's busy watcher covers queued registration/cancellation callbacks.
  GObject *busy = nm_secret_agent_old_get_context_busy_watcher(b->agent);
  g_object_add_weak_pointer(busy, (gpointer *)&busy);
  g_object_unref(b->agent);
  while (busy)
    g_main_context_iteration(NULL, TRUE);
  // Unregister is fire-and-forget; flush its queued message before exiting.
  g_dbus_connection_flush_sync(connection, NULL, NULL);
  g_object_unref(connection);
  g_source_remove(b->tick);
  sigaction(SIGTERM, &b->term, NULL);
  sigaction(SIGINT, &b->interrupt, NULL);
  g_free(b);
}
