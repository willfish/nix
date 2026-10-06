CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=c17 -Wall -Wextra -Wpedantic -Werror
PREFIX ?= /usr/local

.PHONY: all clean check install
all: memscope
memscope: main.c proc.c render.c memscope.h
	$(CC) $(CPPFLAGS) $(CFLAGS) main.c proc.c render.c $(LDFLAGS) -o $@
test_proc: test_proc.c proc.c memscope.h
	$(CC) $(CPPFLAGS) $(CFLAGS) test_proc.c proc.c $(LDFLAGS) -o $@
test_support: test_support.c
	$(CC) $(CPPFLAGS) $(CFLAGS) test_support.c $(LDFLAGS) -lutil -o $@
check: memscope test_proc test_support
	./test_proc
	node --test tests.test.ts
install: memscope
	install -Dm755 memscope $(DESTDIR)$(PREFIX)/bin/memscope
clean:
	rm -f memscope test_proc test_support
