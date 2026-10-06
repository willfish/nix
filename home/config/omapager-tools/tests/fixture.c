#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>

static PyObject *converted(PyObject *value) {
  if (PyDict_Check(value) && PyDict_Size(value) == 1) {
    PyObject *data = PyDict_GetItemString(value, "$bytes"),
             *path = PyDict_GetItemString(value, "$path");
    if (data) {
      Py_ssize_t n = PyList_Size(data);
      if (n < 0)
        return NULL;
      PyObject *out = PyBytes_FromStringAndSize(NULL, n);
      if (!out)
        return NULL;
      for (Py_ssize_t i = 0; i < n; i++) {
        long c = PyLong_AsLong(PyList_GetItem(data, i));
        if (c < 0 || c > 255 || PyErr_Occurred()) {
          Py_DECREF(out);
          return NULL;
        }
        PyBytes_AS_STRING(out)[i] = (char)c;
      }
      return out;
    }
    if (path) {
      PyObject *module = PyImport_ImportModule("pathlib");
      if (!module)
        return NULL;
      PyObject *ctor = PyObject_GetAttrString(module, "Path");
      Py_DECREF(module);
      if (!ctor)
        return NULL;
      PyObject *out = PyObject_CallOneArg(ctor, path);
      Py_DECREF(ctor);
      return out;
    }
  }
  if (PyList_Check(value)) {
    PyObject *out = PyList_New(PyList_GET_SIZE(value));
    if (!out)
      return NULL;
    for (Py_ssize_t i = 0; i < PyList_GET_SIZE(value); i++) {
      PyObject *item = converted(PyList_GET_ITEM(value, i));
      if (!item) {
        Py_DECREF(out);
        return NULL;
      }
      PyList_SET_ITEM(out, i, item);
    }
    return out;
  }
  if (PyDict_Check(value)) {
    PyObject *out = PyDict_New(), *key, *item;
    Py_ssize_t at = 0;
    if (!out)
      return NULL;
    while (PyDict_Next(value, &at, &key, &item)) {
      PyObject *copy = converted(item);
      if (!copy || PyDict_SetItem(out, key, copy) < 0) {
        Py_XDECREF(copy);
        Py_DECREF(out);
        return NULL;
      }
      Py_DECREF(copy);
    }
    return out;
  }
  return Py_NewRef(value);
}
static PyObject *error_result(void) {
  PyObject *type = NULL, *value = NULL, *trace = NULL;
  PyErr_Fetch(&type, &value, &trace);
  PyObject *name = type ? PyObject_GetAttrString(type, "__name__")
                        : PyUnicode_FromString("UnknownError");
  Py_XDECREF(type);
  Py_XDECREF(value);
  Py_XDECREF(trace);
  if (!name)
    return NULL;
  PyObject *out = PyDict_New();
  if (out && PyDict_SetItemString(out, "error", name) < 0)
    Py_CLEAR(out);
  Py_DECREF(name);
  return out;
}
static int insert_path(const char *path, int at) {
  PyObject *s = PyUnicode_DecodeFSDefault(path);
  if (!s)
    return -1;
  int ok = PyList_Insert(PySys_GetObject("path"), at, s);
  Py_DECREF(s);
  return ok;
}
int main(int argc, char **argv) {
  if (argc < 2 || argc > 3)
    return 2;
  PyConfig config;
  PyConfig_InitIsolatedConfig(&config);
  config.write_bytecode = 0;
  PyStatus status = Py_InitializeFromConfig(&config);
  PyConfig_Clear(&config);
  if (PyStatus_Exception(status))
    Py_ExitStatusException(status);
  PyObject *json = NULL, *loads = NULL, *dumps = NULL, *request = NULL,
           *module = NULL, *globals = NULL, *result = NULL, *fn = NULL,
           *args = NULL, *kwargs = NULL;
  int exit_code = 1;
  if (insert_path(argv[1], 0) < 0)
    goto done;
  if (argc == 3) {
    char *dir = g_path_get_dirname(argv[2]);
    int ok = insert_path(dir, 1);
    g_free(dir);
    if (ok < 0)
      goto done;
  }
  json = PyImport_ImportModule("json");
  loads = json ? PyObject_GetAttrString(json, "loads") : NULL;
  dumps = json ? PyObject_GetAttrString(json, "dumps") : NULL;
  if (!loads || !dumps)
    goto done;
  GString *input = g_string_new(NULL);
  char buffer[8192];
  size_t n;
  while ((n = fread(buffer, 1, sizeof buffer, stdin)))
    g_string_append_len(input, buffer, (gssize)n);
  if (ferror(stdin)) {
    g_string_free(input, TRUE);
    goto done;
  }
  PyObject *bytes =
      PyBytes_FromStringAndSize(input->str, (Py_ssize_t)input->len);
  g_string_free(input, TRUE);
  if (!bytes)
    goto done;
  request = PyObject_CallOneArg(loads, bytes);
  Py_DECREF(bytes);
  if (!request)
    goto done;
  module = PyImport_ImportModule("icon_paths");
  if (!module)
    goto done;
  PyObject *name = PyDict_GetItemString(request, "fn");
  if (!name)
    goto done;
  if (PyUnicode_CompareWithASCIIString(name, "$origin") == 0) {
    result = PyObject_GetAttrString(module, "__file__");
    goto output;
  }
  if (argc == 3) {
    PyObject *runpy = PyImport_ImportModule("runpy"),
             *run = runpy ? PyObject_GetAttrString(runpy, "run_path") : NULL,
             *path = PyUnicode_DecodeFSDefault(argv[2]);
    Py_XDECREF(runpy);
    if (run && path)
      globals = PyObject_CallOneArg(run, path);
    Py_XDECREF(run);
    Py_XDECREF(path);
    if (!globals)
      goto done;
    fn = Py_XNewRef(PyDict_GetItem(globals, name));
  } else
    fn = PyObject_GetAttr(module, name);
  PyObject *raw = PyDict_GetItemString(request, "args"),
           *list = raw ? converted(raw) : PyList_New(0);
  args = list ? PyList_AsTuple(list) : NULL;
  Py_XDECREF(list);
  raw = PyDict_GetItemString(request, "kwargs");
  kwargs = raw ? converted(raw) : PyDict_New();
  if (!fn || !args || !kwargs)
    goto done;
  raw = PyDict_GetItemString(request, "repeat");
  long repeat = raw ? PyLong_AsLong(raw) : 1;
  if (PyErr_Occurred())
    goto done;
  for (long i = 0; i < repeat; i++) {
    Py_CLEAR(result);
    result = PyObject_Call(fn, args, kwargs);
    if (!result) {
      result = error_result();
      break;
    }
  }
output:
  if (!result)
    goto done;
  PyObject *encoded = PyObject_CallOneArg(dumps, result);
  if (!encoded)
    goto done;
  const char *text = PyUnicode_AsUTF8(encoded);
  if (text) {
    puts(text);
    exit_code = 0;
  }
  Py_DECREF(encoded);
done:
  if (PyErr_Occurred())
    PyErr_Print();
  Py_XDECREF(json);
  Py_XDECREF(loads);
  Py_XDECREF(dumps);
  Py_XDECREF(request);
  Py_XDECREF(module);
  Py_XDECREF(globals);
  Py_XDECREF(result);
  Py_XDECREF(fn);
  Py_XDECREF(args);
  Py_XDECREF(kwargs);
  if (Py_FinalizeEx() < 0)
    exit_code = 1;
  return exit_code;
}
