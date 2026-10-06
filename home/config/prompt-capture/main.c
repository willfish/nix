#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "config.h"

/* Owned capture policy, using the pinned upstream mitmproxy/CPython runtime. */
static void drop(PyObject **value) { Py_XDECREF(*value); }
#define OBJ PyObject *__attribute__((cleanup(drop)))
static PyObject *loads, *dumps, *strftime_fn, *run_id, *tool, *dump_class;
static FILE *output;

static PyObject *attr(PyObject *object, const char *name) {
  return object ? PyObject_GetAttrString(object, name) : NULL;
}
static int put(PyObject *dict, const char *key, PyObject *value) {
  return dict && value ? PyDict_SetItemString(dict, key, value) : -1;
}
static int text(PyObject *dict, const char *key, const char *value) {
  OBJ string = PyUnicode_FromString(value);
  return put(dict, key, string);
}
static int field(PyObject *dict, const char *key, PyObject *object,
                 const char *name) {
  OBJ value = attr(object, name);
  return put(dict, key, value);
}
static int count(PyObject *dict, const char *key, Py_ssize_t size) {
  OBJ value = size >= 0 ? PyLong_FromSsize_t(size) : NULL;
  return put(dict, key, value);
}
static int capture_body(void) {
  const char *value = getenv("PROMPT_CAPTURE_RESPONSE_BODY");
  return value && !strcmp(value, "1");
}
static void diagnostic(void) {
  PyErr_Clear();
  fputs("prompt-capture addon error; captured data has not been logged to stderr\n",
        stderr);
  fflush(stderr);
}
static PyObject *clip(PyObject *value) {
  if (!value) return NULL;
  if (value == Py_None) return PyUnicode_FromString("");
  Py_ssize_t length = PyUnicode_GetLength(value);
  if (length < 0) return NULL;
  if (length <= 200000) return Py_NewRef(value);
  OBJ start = PyUnicode_Substring(value, 0, 200000);
  OBJ suffix = PyUnicode_FromFormat("...<truncated %zd chars>", length - 200000);
  return start && suffix ? PyUnicode_Concat(start, suffix) : NULL;
}
static PyObject *record(const char *kind, PyObject *flow) {
  OBJ result = PyDict_New();
  OBJ timestamp = PyObject_CallFunction(strftime_fn, "s", "%Y-%m-%dT%H:%M:%S%z");
  if (put(result, "ts", timestamp) || put(result, "run", run_id) ||
      put(result, "tool", tool) || text(result, "kind", kind) ||
      field(result, "flow_id", flow, "id")) return NULL;
  return Py_NewRef(result);
}
static int url(PyObject *rec, PyObject *request) {
  OBJ host = attr(request, "pretty_host");
  OBJ path = attr(request, "path");
  OBJ joined = host && path ? PyUnicode_Concat(host, path) : NULL;
  return put(rec, "url", joined);
}
static int emit(PyObject *rec, int ascii) {
  if (!output) return 0;
  OBJ args = PyTuple_Pack(1, rec);
  OBJ kwargs = Py_BuildValue("{s:O,s:O}", "ensure_ascii", ascii ? Py_True : Py_False,
                             "default", (PyObject *)&PyUnicode_Type);
  OBJ encoded = args && kwargs ? PyObject_Call(dumps, args, kwargs) : NULL;
  Py_ssize_t size = 0;
  const char *bytes = encoded ? PyUnicode_AsUTF8AndSize(encoded, &size) : NULL;
  if (!bytes) return -1;
  if (fwrite(bytes, 1, (size_t)size, output) != (size_t)size ||
      fputc('\n', output) == EOF || fflush(output)) {
    PyErr_SetFromErrno(PyExc_OSError);
    return -1;
  }
  return 0;
}
static PyObject *parse(PyObject *body) {
  PyObject *result = body ? PyObject_CallOneArg(loads, body) : NULL;
  if (!result && (PyErr_ExceptionMatches(PyExc_ValueError) ||
                  PyErr_ExceptionMatches(PyExc_TypeError))) PyErr_Clear();
  return result;
}
static int usage(PyObject *flow, PyObject *body) {
  OBJ payload = parse(body);
  if (!payload) return PyErr_Occurred() ? -1 : 0;
  if (!PyDict_Check(payload)) return 0;
  PyObject *value = PyDict_GetItemString(payload, "usage");
  const char *keys[] = {"message", "response"};
  for (size_t i = 0; i < 2; i++) {
    PyObject *nested = PyDict_GetItemString(payload, keys[i]);
    if ((!value || !PyDict_Check(value)) && nested && PyDict_Check(nested))
      value = PyDict_GetItemString(nested, "usage");
  }
  if (!output || !value || !PyDict_Check(value)) return 0;
  OBJ rec = record("usage", flow);
  return put(rec, "usage", value) ? -1 : emit(rec, 1);
}
static PyObject *clean(PyObject *headers) {
  static const char *redacted[] = {"authorization", "proxy-authorization",
      "x-api-key", "api-key", "x-openrouter-api-key", "cookie"};
  OBJ result = PyDict_New();
  OBJ items = headers ? PyObject_CallMethod(headers, "items", NULL) : NULL;
  OBJ iterator = items ? PyObject_GetIter(items) : NULL;
  if (!result || !iterator) return NULL;
  PyObject *item;
  while ((item = PyIter_Next(iterator))) {
    OBJ entry = item;
    OBJ key = PySequence_GetItem(entry, 0);
    OBJ value = PySequence_GetItem(entry, 1);
    OBJ lower = key ? PyObject_CallMethod(key, "lower", NULL) : NULL;
    const char *name = lower ? PyUnicode_AsUTF8(lower) : NULL;
    if (!name || !value) return NULL;
    for (size_t i = 0; i < sizeof(redacted) / sizeof(*redacted); i++) {
      if (!strcmp(name, redacted[i])) {
        Py_SETREF(value, PyUnicode_FromString("<redacted>"));
        break;
      }
    }
    if (!value || PyDict_SetItem(result, key, value)) return NULL;
  }
  return PyErr_Occurred() ? NULL : Py_NewRef(result);
}
static int capture(const char *kind, PyObject *flow) {
  if (!output) return 0;
  OBJ req = attr(flow, "request");
  OBJ body = req ? PyObject_CallMethod(req, "get_text", "O", Py_False) : NULL;
  if (!body) return -1;
  if (body == Py_None) Py_SETREF(body, PyUnicode_FromString(""));
  OBJ headers = attr(req, "headers");
  OBJ cleaned = clean(headers);
  OBJ raw = attr(req, "raw_content");
  OBJ rec = record(kind, flow);
  if (!body || !raw || field(rec, "method", req, "method") || url(rec, req) ||
      put(rec, "request_headers", cleaned) || put(rec, "request_body", body) ||
      count(rec, "request_bytes", raw == Py_None ? 0 : PyObject_Length(raw)) ||
      count(rec, "request_chars", PyUnicode_GetLength(body))) return -1;
  OBJ payload = parse(body);
  if (PyErr_Occurred()) return -1;
  if (payload && PyDict_Check(payload)) {
    PyObject *model = PyDict_GetItemString(payload, "model");
    if (put(rec, "model", model ? model : Py_None)) return -1;
    const char *keys[] = {"messages", "tools"};
    const char *labels[] = {"message_count", "tool_count"};
    for (size_t i = 0; i < 2; i++) {
      PyObject *list = PyDict_GetItemString(payload, keys[i]);
      if (list && PyList_Check(list) && count(rec, labels[i], PyList_Size(list)))
        return -1;
    }
  }
  OBJ resp = attr(flow, "response");
  if (!resp) return -1;
  if (resp != Py_None) {
    OBJ stream = attr(resp, "stream");
    int streaming = stream ? PyObject_IsTrue(stream) : -1;
    if (streaming < 0 || field(rec, "status", resp, "status_code")) return -1;
    if (!streaming) {
      OBJ response = PyObject_CallMethod(resp, "get_text", "O", Py_False);
      if (!response || usage(flow, response)) return -1;
      if (capture_body()) {
        OBJ clipped = clip(response);
        if (put(rec, "response_body", clipped)) return -1;
      }
    }
  }
  return emit(rec, 0);
}

/* Each streaming response owns its incremental decoder and pending SSE event. */
typedef struct {
  PyObject_HEAD
  PyObject *flow, *decoder, *pending, *lines;
  int body;
} Stream;
static void stream_free(PyObject *object) {
  Stream *self = (Stream *)object;
  PyObject_GC_UnTrack(self);
  Py_CLEAR(self->flow);
  Py_CLEAR(self->decoder);
  Py_CLEAR(self->pending);
  Py_CLEAR(self->lines);
  Py_TYPE(self)->tp_free(object);
}
static int stream_visit(PyObject *object, visitproc visit, void *arg) {
  Stream *self = (Stream *)object;
  Py_VISIT(self->flow); Py_VISIT(self->decoder);
  Py_VISIT(self->pending); Py_VISIT(self->lines);
  return 0;
}
static int stream_clear(PyObject *object) {
  Stream *self = (Stream *)object;
  Py_CLEAR(self->flow); Py_CLEAR(self->decoder);
  Py_CLEAR(self->pending); Py_CLEAR(self->lines);
  return 0;
}
static int flush_lines(Stream *self) {
  if (!PyList_Size(self->lines)) return 0;
  OBJ separator = PyUnicode_FromString("\n");
  OBJ body = separator ? PyUnicode_Join(separator, self->lines) : NULL;
  if (!body || usage(self->flow, body)) return -1;
  return PyList_SetSlice(self->lines, 0, PyList_Size(self->lines), NULL);
}
static int data_line(Stream *self, PyObject *line) {
  OBJ prefix = PyUnicode_FromString("data:");
  int starts = prefix ? PyUnicode_Tailmatch(line, prefix, 0, PY_SSIZE_T_MAX, -1) : -1;
  if (starts < 0) return -1;
  if (!starts) return 0;
  OBJ body = PyUnicode_Substring(line, 5, PyUnicode_GetLength(line));
  OBJ trimmed = body ? PyObject_CallMethod(body, "lstrip", "s", " ") : NULL;
  return trimmed ? PyList_Append(self->lines, trimmed) : -1;
}
static int stream_capture(Stream *self, PyObject *chunk) {
  int final = !PyObject_IsTrue(chunk);
  OBJ decoded = PyObject_CallMethod(self->decoder, "decode", "Oi", chunk, final);
  OBJ pending = decoded ? PyUnicode_Concat(self->pending, decoded) : NULL;
  OBJ separator = PyUnicode_FromString("\n");
  OBJ parts = pending && separator ? PyUnicode_Split(pending, separator, -1) : NULL;
  if (!parts) return -1;
  Py_ssize_t size = PyList_Size(parts);
  Py_SETREF(self->pending, Py_NewRef(PyList_GetItem(parts, size - 1)));
  for (Py_ssize_t i = 0; i < size - 1; i++) {
    OBJ line = PyObject_CallMethod(PyList_GetItem(parts, i), "rstrip", "s", "\r");
    if (!line) return -1;
    if (PyUnicode_GetLength(line) == 0) {
      if (flush_lines(self)) return -1;
    } else if (data_line(self, line)) return -1;
  }
  if (final) {
    if (data_line(self, self->pending) || flush_lines(self)) return -1;
    Py_SETREF(self->pending, PyUnicode_FromString(""));
    if (!self->pending) return -1;
  }
  if (output && PyUnicode_GetLength(decoded) && self->body) {
    OBJ rec = record("response_chunk", self->flow);
    OBJ req = attr(self->flow, "request");
    OBJ resp = attr(self->flow, "response");
    OBJ clipped = clip(decoded);
    if (field(rec, "method", req, "method") || url(rec, req) ||
        field(rec, "status", resp, "status_code") ||
        put(rec, "response_body", clipped) || emit(rec, 0)) return -1;
  }
  return 0;
}
static PyObject *stream_call(PyObject *object, PyObject *args, PyObject *kwargs) {
  (void)kwargs;
  PyObject *chunk;
  if (!PyArg_ParseTuple(args, "O", &chunk)) return NULL;
  if (stream_capture((Stream *)object, chunk)) diagnostic();
  /* Capture failures must never alter or buffer the caller's original bytes. */
  return Py_NewRef(chunk);
}
static PyTypeObject StreamType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "prompt_capture.Stream",
    .tp_basicsize = sizeof(Stream),
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_call = stream_call,
    .tp_dealloc = stream_free,
    .tp_traverse = stream_visit,
    .tp_clear = stream_clear,
};
static int response_headers(PyObject *flow) {
  OBJ resp = attr(flow, "response");
  OBJ headers = attr(resp, "headers");
  OBJ content = headers ? PyObject_CallMethod(headers, "get", "ss", "content-type", "") : NULL;
  OBJ parts = content ? PyObject_CallMethod(content, "split", "si", ";", 1) : NULL;
  OBJ stripped = parts ? PyObject_CallMethod(PyList_GetItem(parts, 0), "strip", NULL) : NULL;
  OBJ lower = stripped ? PyObject_CallMethod(stripped, "lower", NULL) : NULL;
  const char *type = lower ? PyUnicode_AsUTF8(lower) : NULL;
  if (!type) return -1;
  if (strcmp(type, "text/event-stream")) return 0;
  OBJ codecs = PyImport_ImportModule("codecs");
  OBJ factory = codecs ? PyObject_CallMethod(codecs, "getincrementaldecoder", "s", "utf-8") : NULL;
  OBJ object = StreamType.tp_alloc(&StreamType, 0);
  if (!object) return -1;
  Stream *stream = (Stream *)object;
  stream->flow = Py_NewRef(flow);
  stream->decoder = factory ? PyObject_CallFunction(factory, "s", "replace") : NULL;
  stream->pending = PyUnicode_FromString("");
  stream->lines = PyList_New(0);
  stream->body = capture_body();
  if (!stream->decoder || !stream->pending || !stream->lines) return -1;
  return PyObject_SetAttrString(resp, "stream", object);
}
static int websocket(PyObject *flow) {
  if (!output) return 0;
  OBJ ws = attr(flow, "websocket");
  if (!ws) return -1;
  if (ws == Py_None) return 0;
  OBJ messages = attr(ws, "messages");
  OBJ message = messages ? PySequence_GetItem(messages, -1) : NULL;
  OBJ is_text = attr(message, "is_text");
  if (!is_text) return -1;
  OBJ payload = NULL;
  if (PyObject_IsTrue(is_text)) payload = attr(message, "text");
  else {
    OBJ type = attr(message, "type");
    OBJ name = attr(type, "name");
    if (!name) return -1;
    if (PyUnicode_CompareWithASCIIString(name, "BINARY")) return 0;
    OBJ content = attr(message, "content");
    payload = content ? PyObject_Repr(content) : NULL;
  }
  OBJ client = attr(message, "from_client");
  if (!client || !payload) return -1;
  OBJ rec = record(PyObject_IsTrue(client) ? "ws_request" : "ws_response", flow);
  OBJ req = attr(flow, "request");
  OBJ clipped = clip(payload);
  return text(rec, "method", "WS") || url(rec, req) ||
         put(rec, "data", clipped) ? -1 : emit(rec, 0);
}
#define HOOK(name, expression) \
  static PyObject *name(PyObject *self, PyObject *flow) { \
    (void)self; \
    if ((expression)) diagnostic(); \
    Py_RETURN_NONE; \
  }
HOOK(on_request, capture("request", flow))
HOOK(on_response, capture("response", flow))
HOOK(on_headers, response_headers(flow))
HOOK(on_websocket, websocket(flow))
static PyMethodDef methods[] = {
    {"request", on_request, METH_O, NULL},
    {"response", on_response, METH_O, NULL},
    {"responseheaders", on_headers, METH_O, NULL},
    {"websocket_message", on_websocket, METH_O, NULL},
    {NULL, NULL, 0, NULL},
};
static PyTypeObject AddonType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "prompt_capture.PromptCapture",
    .tp_basicsize = sizeof(PyObject),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_methods = methods,
};
/* mitmproxy's script loader only accepts Python source. Register the native
 * object when constructing DumpMaster instead, then use its normal CLI/event loop. */
static PyObject *master_factory(PyObject *self, PyObject *args) {
  (void)self;
  OBJ master = PyObject_CallObject(dump_class, args);
  OBJ manager = attr(master, "addons");
  OBJ addon = AddonType.tp_alloc(&AddonType, 0);
  OBJ result = manager && addon ? PyObject_CallMethod(manager, "add", "O", addon) : NULL;
  return result ? Py_NewRef(master) : NULL;
}
static PyMethodDef factory_def = {"capture_master", master_factory, METH_VARARGS, NULL};
static int initialize(void) {
  PyConfig config;
  PyConfig_InitIsolatedConfig(&config);
  config.install_signal_handlers = 1;
  config.write_bytecode = 0;
  PyStatus status = PyConfig_SetBytesString(&config, &config.program_name, PYTHON_EXECUTABLE);
  if (!PyStatus_Exception(status)) status = Py_InitializeFromConfig(&config);
  PyConfig_Clear(&config);
  if (PyStatus_Exception(status)) return -1;
  OBJ site = PyImport_ImportModule("site");
  OBJ added = site ? PyObject_CallMethod(site, "addsitedir", "s", SITE_PACKAGES) : NULL;
  OBJ json = PyImport_ImportModule("json");
  OBJ time = PyImport_ImportModule("time");
  loads = attr(json, "loads"); dumps = attr(json, "dumps");
  strftime_fn = attr(time, "strftime");
  run_id = PyUnicode_FromString(getenv("PROMPT_CAPTURE_RUN") ? getenv("PROMPT_CAPTURE_RUN") : "");
  tool = PyUnicode_FromString(getenv("PROMPT_CAPTURE_TOOL") ? getenv("PROMPT_CAPTURE_TOOL") : "unknown");
  if (!added || !loads || !dumps || !strftime_fn || !run_id || !tool ||
      PyType_Ready(&StreamType) || PyType_Ready(&AddonType)) return -1;
  const char *path = getenv("PROMPT_CAPTURE_JSONL");
  if (path && *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    output = fdopen(fd, "a");
    if (!output) { close(fd); return -1; }
  }
  return 0;
}
static int run(int argc, char **argv) {
  OBJ main = PyImport_ImportModule("mitmproxy.tools.main");
  OBJ dump = PyImport_ImportModule("mitmproxy.tools.dump");
  OBJ cmdline = PyImport_ImportModule("mitmproxy.tools.cmdline");
  dump_class = attr(dump, "DumpMaster");
  OBJ parser = attr(cmdline, "mitmdump");
  OBJ factory = PyCFunction_New(&factory_def, NULL);
  OBJ args = PyList_New(0);
  if (!main || !dump_class || !parser || !factory || !args) return -1;
  for (int i = 1; i < argc; i++) {
    OBJ arg = PyUnicode_DecodeFSDefault(argv[i]);
    if (!arg || PyList_Append(args, arg)) return -1;
  }
  OBJ result = PyObject_CallMethod(main, "run", "OOO", factory, parser, args);
  return result ? 0 : -1;
}
int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "--help")) {
    puts("prompt-capture-mitm: private capture adapter for the prompt-capture wrapper");
    return 0;
  }
  int status = initialize() || run(argc, argv) ? 1 : 0;
  if (status) {
    if (Py_IsInitialized() && PyErr_Occurred()) PyErr_Clear();
    fputs("prompt-capture: native capture adapter failed; private data not logged\n", stderr);
  }
  Py_CLEAR(loads); Py_CLEAR(dumps); Py_CLEAR(strftime_fn);
  Py_CLEAR(run_id); Py_CLEAR(tool); Py_CLEAR(dump_class);
  if (Py_IsInitialized() && Py_FinalizeEx() < 0) status = 120;
  if (output && fclose(output)) status = 1;
  return status;
}
