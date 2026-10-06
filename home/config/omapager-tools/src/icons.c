#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
  PyObject *path, *os;
} State;
static State *state(PyObject *module) { return PyModule_GetState(module); }
static PyObject *call(PyObject *object, const char *name, PyObject *arg) {
  PyObject *fn = PyObject_GetAttrString(object, name);
  if (!fn)
    return NULL;
  PyObject *out = PyObject_CallOneArg(fn, arg);
  Py_DECREF(fn);
  return out;
}
static PyObject *text(PyObject *value) {
  int yes = PyObject_IsTrue(value);
  if (yes < 0)
    return NULL;
  return yes ? PyObject_Str(value) : PyUnicode_FromString("");
}
static PyObject *lower(PyObject *s) {
  return PyObject_CallMethod(s, "lower", NULL);
}
static PyObject *slug(PyObject *value) {
  PyObject *s = text(value);
  if (!s)
    return NULL;
  Py_ssize_t n = PyUnicode_GetLength(s);
  PyObject *slice = PyUnicode_Substring(s, 0, n < 256 ? n : 256);
  Py_DECREF(s);
  if (!slice)
    return NULL;
  PyObject *folded = lower(slice);
  Py_DECREF(slice);
  if (!folded)
    return NULL;
  n = PyUnicode_GetLength(folded);
  char *buf = PyMem_Malloc((size_t)n + 1);
  if (!buf) {
    Py_DECREF(folded);
    return PyErr_NoMemory();
  }
  Py_ssize_t out = 0;
  int bad = 0;
  for (Py_ssize_t i = 0; i < n; i++) {
    Py_UCS4 c = PyUnicode_ReadChar(folded, i);
    int allowed = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '-';
    if (allowed) {
      buf[out++] = (char)c;
      bad = 0;
    } else {
      if (!bad)
        buf[out++] = '-';
      bad = 1;
    }
  }
  Py_ssize_t start = 0;
  while (start < out && (buf[start] == '-' || buf[start] == '.'))
    start++;
  while (out > start && (buf[out - 1] == '-' || buf[out - 1] == '.'))
    out--;
  Py_ssize_t width = out - start;
  if (width > 100)
    width = 100;
  PyObject *result = PyUnicode_FromStringAndSize(buf + start, width);
  PyMem_Free(buf);
  Py_DECREF(folded);
  return result;
}
static PyObject *compact(PyObject *s) {
  Py_ssize_t n = PyUnicode_GetLength(s);
  char *buf = PyMem_Malloc((size_t)n + 1);
  if (!buf)
    return PyErr_NoMemory();
  Py_ssize_t out = 0;
  for (Py_ssize_t i = 0; i < n; i++) {
    Py_UCS4 c = PyUnicode_ReadChar(s, i);
    if (c != '-' && c != '.')
      buf[out++] = (char)c;
  }
  PyObject *result = PyUnicode_FromStringAndSize(buf, out);
  PyMem_Free(buf);
  return result;
}
static int append(PyObject *out, PyObject *seen, PyObject *candidate) {
  int yes = PyObject_IsTrue(candidate);
  if (yes <= 0)
    return yes;
  yes = PySet_Contains(seen, candidate);
  if (yes < 0)
    return -1;
  if (yes)
    return 0;
  if (PySet_Add(seen, candidate) < 0)
    return -1;
  return PyList_Append(out, candidate);
}
static const struct {
  const char *key, *first, *second;
} aliases[] = {{"discord", "discord", NULL},
               {"github", "github", NULL},
               {"github-notifications", "github", NULL},
               {"org.telegram.desktop", "org.telegram.desktop", NULL},
               {"telegram", "org.telegram.desktop", "telegram"},
               {"telegram-desktop", "org.telegram.desktop", "telegram"},
               {"telegramdesktop", "org.telegram.desktop", "telegram"},
               {"whatsapp", "whatsapp", NULL},
               {"whats-app", "whatsapp", NULL}};
static PyObject *expand_names(PyObject *self, PyObject *names) {
  (void)self;
  PyObject *out = PyList_New(0), *seen = PySet_New(NULL), *iter = NULL,
           *item = NULL, *raw = NULL, *folded = NULL, *token = NULL,
           *dense = NULL;
  if (!out || !seen)
    goto error;
  int yes = PyObject_IsTrue(names);
  if (yes < 0)
    goto error;
  if (!yes) {
    Py_DECREF(seen);
    return out;
  }
  iter = PyObject_GetIter(names);
  if (!iter)
    goto error;
  while ((item = PyIter_Next(iter))) {
    PyObject *s = text(item);
    Py_CLEAR(item);
    if (!s)
      goto error;
    raw = PyObject_CallMethod(s, "strip", NULL);
    Py_DECREF(s);
    if (!raw)
      goto error;
    if (PyUnicode_GetLength(raw) == 0) {
      Py_CLEAR(raw);
      continue;
    }
    token = slug(raw);
    folded = lower(raw);
    dense = token ? compact(token) : NULL;
    if (!token || !folded || !dense)
      goto error;
    if (append(out, seen, raw) < 0 || append(out, seen, token) < 0)
      goto error;
    PyObject *keys[] = {raw, folded, token, dense};
    for (size_t k = 0; k < 4; k++)
      for (size_t j = 0; j < sizeof(aliases) / sizeof(aliases[0]); j++) {
        int equal = PyUnicode_CompareWithASCIIString(keys[k], aliases[j].key);
        if (equal == -1 && PyErr_Occurred())
          goto error;
        if (equal)
          continue;
        const char *values[] = {aliases[j].first, aliases[j].second};
        for (size_t v = 0; v < 2 && values[v]; v++) {
          PyObject *candidate = PyUnicode_FromString(values[v]);
          if (!candidate)
            goto error;
          int ok = append(out, seen, candidate);
          Py_DECREF(candidate);
          if (ok < 0)
            goto error;
        }
      }
    Py_CLEAR(raw);
    Py_CLEAR(folded);
    Py_CLEAR(token);
    Py_CLEAR(dense);
  }
  if (PyErr_Occurred())
    goto error;
  Py_DECREF(iter);
  Py_DECREF(seen);
  return out;
error:
  Py_XDECREF(item);
  Py_XDECREF(raw);
  Py_XDECREF(folded);
  Py_XDECREF(token);
  Py_XDECREF(dense);
  Py_XDECREF(iter);
  Py_XDECREF(seen);
  Py_XDECREF(out);
  return NULL;
}
static PyObject *pieces(PyObject *s, int whitespace) {
  PyObject *list = PyList_New(0);
  if (!list)
    return NULL;
  Py_ssize_t start = 0, n = PyUnicode_GetLength(s);
  for (Py_ssize_t i = 0; i <= n; i++) {
    Py_UCS4 c = i < n ? PyUnicode_ReadChar(s, i) : '-';
    int separator =
        c == '-' ||
        (whitespace && (c == '.' || c == '_' || Py_UNICODE_ISSPACE(c)));
    if (!separator)
      continue;
    if (i > start) {
      PyObject *part = PyUnicode_Substring(s, start, i);
      if (!part || PyList_Append(list, part) < 0) {
        Py_XDECREF(part);
        Py_DECREF(list);
        return NULL;
      }
      Py_DECREF(part);
    }
    start = i + 1;
  }
  return list;
}
static PyObject *name_matches(PyObject *self, PyObject *args,
                              PyObject *kwargs) {
  (void)self;
  PyObject *want, *haystack;
  char *keys[] = {"want", "haystack", NULL};
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "OO:name_matches", keys, &want,
                                   &haystack))
    return NULL;
  PyObject *token = slug(want), *dense = token ? compact(token) : NULL,
           *s = NULL, *folded = NULL, *words = NULL, *parts = NULL;
  int matched = 0;
  if (!token || !dense)
    goto error;
  if (PyUnicode_GetLength(dense) < 3)
    goto done;
  s = text(haystack);
  folded = s ? lower(s) : NULL;
  words = folded ? pieces(folded, 1) : NULL;
  if (!s || !folded || !words)
    goto error;
  matched = PySequence_Contains(words, token);
  if (matched < 0)
    goto error;
  if (!matched) {
    matched = PySequence_Contains(words, dense);
    if (matched < 0)
      goto error;
  }
  if (matched)
    goto done;
  parts = pieces(token, 0);
  if (!parts)
    goto error;
  Py_ssize_t width = PyList_GET_SIZE(parts), n = PyList_GET_SIZE(words);
  if (width < 2)
    goto done;
  for (Py_ssize_t i = 0; i + width <= n; i++) {
    matched = 1;
    for (Py_ssize_t j = 0; j < width; j++) {
      int equal = PyObject_RichCompareBool(PyList_GET_ITEM(words, i + j),
                                           PyList_GET_ITEM(parts, j), Py_EQ);
      if (equal < 0)
        goto error;
      if (!equal) {
        matched = 0;
        break;
      }
    }
    if (matched)
      break;
  }
done:
  Py_XDECREF(token);
  Py_XDECREF(dense);
  Py_XDECREF(s);
  Py_XDECREF(folded);
  Py_XDECREF(words);
  Py_XDECREF(parts);
  return PyBool_FromLong(matched);
error:
  Py_XDECREF(token);
  Py_XDECREF(dense);
  Py_XDECREF(s);
  Py_XDECREF(folded);
  Py_XDECREF(words);
  Py_XDECREF(parts);
  return NULL;
}
static int inside(State *ctx, PyObject *path, PyObject *root) {
  PyObject *items = PyList_New(2);
  if (!items)
    return -1;
  Py_INCREF(path);
  Py_INCREF(root);
  PyList_SET_ITEM(items, 0, path);
  PyList_SET_ITEM(items, 1, root);
  PyObject *common = call(ctx->path, "commonpath", items);
  Py_DECREF(items);
  if (!common) {
    if (PyErr_ExceptionMatches(PyExc_ValueError)) {
      PyErr_Clear();
      return 0;
    }
    return -1;
  }
  int yes = PyObject_RichCompareBool(common, root, Py_EQ);
  Py_DECREF(common);
  return yes;
}
static PyObject *inside_method(PyObject *self, PyObject *args,
                               PyObject *kwargs) {
  PyObject *path, *root;
  char *keys[] = {"path", "root", NULL};
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "OO:_inside", keys, &path,
                                   &root))
    return NULL;
  int yes = inside(state(self), path, root);
  return yes < 0 ? NULL : PyBool_FromLong(yes);
}
static PyObject *slug_method(PyObject *self, PyObject *args, PyObject *kwargs) {
  (void)self;
  PyObject *value;
  char *keys[] = {"text", NULL};
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O:_slug", keys, &value))
    return NULL;
  return slug(value);
}
static PyObject *expand_method(PyObject *self, PyObject *args,
                               PyObject *kwargs) {
  PyObject *names;
  char *keys[] = {"names", NULL};
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O:expand_names", keys,
                                   &names))
    return NULL;
  return expand_names(self, names);
}
static PyObject *allowed_icon(PyObject *self, PyObject *args,
                              PyObject *kwargs) {
  PyObject *path, *bases;
  char *keys[] = {"path", "bases", NULL};
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "OO:allowed_icon", keys, &path,
                                   &bases))
    return NULL;
  State *ctx = state(self);
  PyObject *file = NULL, *logical = NULL, *real = NULL, *info = NULL,
           *mode = NULL, *iter = NULL, *base = NULL, *root = NULL,
           *real_root = NULL, *result = NULL;
  int yes = PyObject_IsTrue(path);
  if (yes < 0)
    goto done;
  if (!yes) {
    result = Py_NewRef(Py_False);
    goto done;
  }
  file = call(ctx->path, "isfile", path);
  if (!file)
    goto done;
  yes = PyObject_IsTrue(file);
  if (yes < 0)
    goto done;
  if (!yes) {
    result = Py_NewRef(Py_False);
    goto done;
  }
  logical = call(ctx->path, "abspath", path);
  if (!logical)
    goto done;
  real = call(ctx->path, "realpath", path);
  if (!real)
    goto done;
  info = call(ctx->os, "stat", real);
  mode = info ? PyObject_GetAttrString(info, "st_mode") : NULL;
  if (!mode)
    goto done;
  long bits = PyLong_AsLong(mode);
  if (bits == -1 && PyErr_Occurred())
    goto done;
  if (!S_ISREG(bits)) {
    result = Py_NewRef(Py_False);
    goto done;
  }
  iter = PyObject_GetIter(bases);
  if (!iter)
    goto done;
  while ((base = PyIter_Next(iter))) {
    root = call(ctx->path, "abspath", base);
    if (!root)
      goto done;
    yes = inside(ctx, logical, root);
    if (yes < 0)
      goto done;
    if (yes) {
      real_root = call(ctx->path, "realpath", base);
      if (!real_root)
        goto done;
      yes = inside(ctx, real, real_root);
      if (yes < 0)
        goto done;
      if (!yes) {
        PyObject *prefix = PyUnicode_FromString("/nix/store/");
        if (!prefix)
          goto done;
        PyObject *starts = PyObject_CallMethod(real, "startswith", "O", prefix);
        Py_DECREF(prefix);
        if (!starts)
          goto done;
        yes = PyObject_IsTrue(starts);
        Py_DECREF(starts);
        if (yes < 0)
          goto done;
      }
      if (yes) {
        result = Py_NewRef(Py_True);
        goto done;
      }
    }
    Py_CLEAR(base);
    Py_CLEAR(root);
    Py_CLEAR(real_root);
  }
  if (!PyErr_Occurred())
    result = Py_NewRef(Py_False);
done:
  Py_XDECREF(file);
  Py_XDECREF(logical);
  Py_XDECREF(real);
  Py_XDECREF(info);
  Py_XDECREF(mode);
  Py_XDECREF(iter);
  Py_XDECREF(base);
  Py_XDECREF(root);
  Py_XDECREF(real_root);
  return result;
}
static PyMethodDef methods[] = {
    {"expand_names", _PyCFunction_CAST(expand_method),
     METH_VARARGS | METH_KEYWORDS, NULL},
    {"name_matches", _PyCFunction_CAST(name_matches),
     METH_VARARGS | METH_KEYWORDS, NULL},
    {"allowed_icon", _PyCFunction_CAST(allowed_icon),
     METH_VARARGS | METH_KEYWORDS, NULL},
    {"_slug", _PyCFunction_CAST(slug_method), METH_VARARGS | METH_KEYWORDS,
     NULL},
    {"_inside", _PyCFunction_CAST(inside_method), METH_VARARGS | METH_KEYWORDS,
     NULL},
    {NULL, NULL, 0, NULL}};
static int traverse(PyObject *module, visitproc visit, void *arg) {
  State *ctx = state(module);
  Py_VISIT(ctx->path);
  Py_VISIT(ctx->os);
  return 0;
}
static int clear(PyObject *module) {
  State *ctx = state(module);
  Py_CLEAR(ctx->path);
  Py_CLEAR(ctx->os);
  return 0;
}
static void release(void *module) { clear(module); }
static struct PyModuleDef definition = {
    PyModuleDef_HEAD_INIT,
    .m_name = "icon_paths",
    .m_doc = "Compiled icon aliases and confined Nix profile paths.",
    .m_size = sizeof(State),
    .m_methods = methods,
    .m_traverse = traverse,
    .m_clear = clear,
    .m_free = release};
PyMODINIT_FUNC PyInit_icon_paths(void) {
  PyObject *module = PyModule_Create(&definition);
  if (!module)
    return NULL;
  State *ctx = state(module);
  ctx->path = PyImport_ImportModule("os.path");
  ctx->os = PyImport_ImportModule("os");
  if (!ctx->path || !ctx->os) {
    Py_DECREF(module);
    return NULL;
  }
  return module;
}
