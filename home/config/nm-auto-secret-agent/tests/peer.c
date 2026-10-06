#define _POSIX_C_SOURCE 200809L
#include <gio/gio.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static const char xml[] =
    "<node><interface "
    "name='org.freedesktop.NetworkManager.AgentManager'><method "
    "name='RegisterWithCapabilities'><arg type='s' direction='in'/><arg "
    "type='u' direction='in'/></method><method "
    "name='Unregister'/></interface></node>";
static GMainLoop *loop;
static GDBusConnection *bus;
static char *agent;
static gboolean deny;
static void method(GDBusConnection *c, const gchar *sender, const gchar *path,
                   const gchar *iface, const gchar *name, GVariant *parameters,
                   GDBusMethodInvocation *context, gpointer data) {
  (void)c;
  (void)path;
  (void)iface;
  (void)data;
  if (!strcmp(name, "RegisterWithCapabilities")) {
    const char *id;
    guint32 caps;
    g_variant_get(parameters, "(&su)", &id, &caps);
    g_free(agent);
    agent = g_strdup(sender);
    printf("REGISTER\t%s\t%s\t%u\n", sender, id, caps);
    fflush(stdout);
    if (deny) {
      g_dbus_method_invocation_return_dbus_error(
          context,
          "org.freedesktop.NetworkManager.AgentManager.PermissionDenied",
          "fixture denied registration");
      return;
    }
  } else {
    puts("UNREGISTER");
    fflush(stdout);
  }
  g_dbus_method_invocation_return_value(context, NULL);
}
static void called(GObject *object, GAsyncResult *result, gpointer data) {
  (void)data;
  GError *error = NULL;
  GVariant *reply =
      g_dbus_connection_call_finish(G_DBUS_CONNECTION(object), result, &error);
  if (reply) {
    char *s = g_variant_print(reply, TRUE);
    printf("REPLY\t%s\n", s);
    g_free(s);
    g_variant_unref(reply);
  } else {
    char *remote = g_dbus_error_get_remote_error(error);
    printf("ERROR\t%s\t%s\n", remote ? remote : "", error->message);
    g_free(remote);
    g_error_free(error);
  }
  fflush(stdout);
}
static void own(gboolean enable) {
  GError *error = NULL;
  GVariant *result = g_dbus_connection_call_sync(
      bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", enable ? "RequestName" : "ReleaseName",
      enable ? g_variant_new("(su)", "org.freedesktop.NetworkManager", 0u)
             : g_variant_new("(s)", "org.freedesktop.NetworkManager"),
      G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &error);
  if (!result) {
    fprintf(stderr, "peer name: %s\n", error->message);
    g_error_free(error);
    g_main_loop_quit(loop);
    return;
  }
  g_variant_unref(result);
  printf("%s\n", enable ? "OWNED" : "RELEASED");
  fflush(stdout);
}
static gboolean input(GIOChannel *channel, GIOCondition condition,
                      gpointer data) {
  (void)data;
  (void)condition;
  char *line = NULL;
  gsize length;
  GError *error = NULL;
  GIOStatus status =
      g_io_channel_read_line(channel, &line, &length, NULL, &error);
  if (status != G_IO_STATUS_NORMAL) {
    g_clear_error(&error);
    g_free(line);
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
  }
  g_strchomp(line);
  char **fields = g_strsplit(line, "\t", 3);
  if (!strcmp(fields[0], "call") && fields[1] && fields[2]) {
    GVariant *args = g_variant_parse(NULL, fields[2], NULL, NULL, &error);
    if (!args) {
      fprintf(stderr, "variant: %s\n", error->message);
      g_error_free(error);
      g_main_loop_quit(loop);
    } else
      g_dbus_connection_call(
          bus, agent, "/org/freedesktop/NetworkManager/SecretAgent",
          "org.freedesktop.NetworkManager.SecretAgent", fields[1], args, NULL,
          G_DBUS_CALL_FLAGS_NONE, 5000, NULL, called, NULL);
    if (args)
      g_variant_unref(args);
  } else if (!strcmp(fields[0], "target") && fields[1]) {
    g_free(agent);
    agent = g_strdup(fields[1]);
    puts("TARGET");
    fflush(stdout);
  } else if (!strcmp(fields[0], "deny")) {
    deny = TRUE;
    puts("DENY");
    fflush(stdout);
  } else if (!strcmp(fields[0], "own"))
    own(TRUE);
  else if (!strcmp(fields[0], "release"))
    own(FALSE);
  else if (!strcmp(fields[0], "names"))
    g_dbus_connection_call(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                           "org.freedesktop.DBus", "ListNames", NULL, NULL,
                           G_DBUS_CALL_FLAGS_NONE, 3000, NULL, called, NULL);
  else if (!strcmp(fields[0], "quit"))
    g_main_loop_quit(loop);
  else {
    fprintf(stderr, "unknown command\n");
    g_main_loop_quit(loop);
  }
  g_strfreev(fields);
  g_free(line);
  return G_SOURCE_CONTINUE;
}
int main(void) {
  GError *error = NULL;
  bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
  if (!bus) {
    fprintf(stderr, "peer bus: %s\n", error->message);
    g_error_free(error);
    return 1;
  }
  GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(xml, &error);
  if (!info)
    return 1;
  const GDBusInterfaceVTable vtable = {.method_call = method};
  guint object = g_dbus_connection_register_object(
      bus, "/org/freedesktop/NetworkManager/AgentManager", info->interfaces[0],
      &vtable, NULL, NULL, &error);
  if (!object)
    return 1;
  loop = g_main_loop_new(NULL, FALSE);
  GIOChannel *stdin_channel = g_io_channel_unix_new(STDIN_FILENO);
  guint source = g_io_add_watch(stdin_channel, G_IO_IN | G_IO_HUP, input, NULL);
  puts("READY");
  fflush(stdout);
  g_main_loop_run(loop);
  if (g_main_context_find_source_by_id(NULL, source))
    g_source_remove(source);
  g_io_channel_unref(stdin_channel);
  g_dbus_connection_unregister_object(bus, object);
  g_dbus_node_info_unref(info);
  g_main_loop_unref(loop);
  g_free(agent);
  g_object_unref(bus);
  return 0;
}
