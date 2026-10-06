#define PY_SSIZE_T_CLEAN
#include "config.h"
#include <Python.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int initialize(void) {
  PyConfig config;
  PyConfig_InitIsolatedConfig(&config);
  config.install_signal_handlers = 1;
  PyStatus status =
      PyConfig_SetBytesString(&config, &config.program_name, TELEGRAM_PYTHON);
  if (!PyStatus_Exception(status))
    status = Py_InitializeFromConfig(&config);
  PyConfig_Clear(&config);
  if (PyStatus_Exception(status))
    return 1;
  PyObject *site = PyImport_ImportModule("site");
  PyObject *added =
      site ? PyObject_CallMethod(site, "addsitedir", "s", TELEGRAM_SITE) : NULL;
  Py_XDECREF(site);
  if (!added) {
    PyErr_Clear();
    return 1;
  }
  Py_DECREF(added);
  return 0;
}

static int login(PyObject *constructor) {
  const char *session = getenv("TELEGRAM_SESSION_NAME");
  const char *api_id = getenv("TELEGRAM_API_ID");
  const char *api_hash = getenv("TELEGRAM_API_HASH");
  if (!session || !api_id || !api_hash)
    return 1;
  PyObject *name = PyUnicode_DecodeFSDefault(session);
  PyObject *id_text = PyUnicode_DecodeFSDefault(api_id);
  PyObject *id = id_text ? PyLong_FromUnicodeObject(id_text, 10) : NULL;
  PyObject *hash = PyUnicode_DecodeFSDefault(api_hash);
  PyObject *client =
      name && id && hash
          ? PyObject_CallFunctionObjArgs(constructor, name, id, hash, NULL)
          : NULL;
  Py_XDECREF(name);
  Py_XDECREF(id_text);
  Py_XDECREF(id);
  Py_XDECREF(hash);
  if (!client) {
    PyErr_Clear();
    return 1;
  }

  PyObject *started = PyObject_CallMethod(client, "start", NULL);
  PyObject *me = started ? PyObject_CallMethod(client, "get_me", NULL) : NULL;
  PyObject *first = me ? PyObject_GetAttrString(me, "first_name") : NULL;
  PyObject *user = first ? PyObject_GetAttrString(me, "username") : NULL;
  PyObject *message =
      user ? PyUnicode_FromFormat("Logged in as %S (@%S)\n", first, user)
           : NULL;
  int result = message ? PyFile_WriteObject(message, PySys_GetObject("stdout"),
                                            Py_PRINT_RAW)
                       : -1;
  Py_XDECREF(message);
  Py_XDECREF(user);
  Py_XDECREF(first);
  Py_XDECREF(me);
  Py_XDECREF(started);
  // As in the original finally block, disconnect after every created client.
  int interrupted = PyErr_ExceptionMatches(PyExc_KeyboardInterrupt);
  PyErr_Clear();
  PyObject *disconnected = PyObject_CallMethod(client, "disconnect", NULL);
  if (!disconnected)
    result = -1;
  Py_XDECREF(disconnected);
  Py_DECREF(client);
  PyErr_Clear();
  return result == 0 ? 0 : interrupted ? 130 : 1;
}

int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "--help")) {
    puts("telegram-mcp-login: interactively authorize the configured "
         "file-based Telethon session");
    return 0;
  }
  int status = initialize();
  if (!status) {
    PyObject *module = PyImport_ImportModule("telethon.sync");
    PyObject *constructor =
        module ? PyObject_GetAttrString(module, "TelegramClient") : NULL;
    status = constructor ? login(constructor) : 1;
    Py_XDECREF(constructor);
    Py_XDECREF(module);
    PyErr_Clear();
  }
  if (status)
    fputs("Telegram login failed; credential values have not been logged.\n",
          stderr);
  if (Py_IsInitialized() && Py_FinalizeEx() < 0)
    return 120;
  return status;
}
