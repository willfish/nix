#pragma once
#include <stddef.h>
#include <stdint.h>
typedef struct Bridge Bridge;
typedef struct {
  void (*get)(void *, const char *, const char *, const char *, const char *,
              uint32_t, char **, char **);
  void (*cancel)(void *, const char *, const char *);
  void (*registered)(void *, int, const char *);
  void (*release)(char *);
} Callbacks;
Bridge *bridge_new(void *, const Callbacks *, char **);
void bridge_start(Bridge *);
void bridge_step(void);
void bridge_destroy(Bridge *);
void bridge_free_error(char *);
int bridge_signal(void);
int bridge_nonblock(int);
int bridge_printable(uint32_t);
