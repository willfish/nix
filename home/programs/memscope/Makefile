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
test_cli: test_cli.c
	$(CC) $(CPPFLAGS) $(CFLAGS) test_cli.c $(LDFLAGS) -o $@
check: memscope test_proc test_support test_cli
	./test_proc
	./test_cli
install: memscope
	install -Dm755 memscope $(DESTDIR)$(PREFIX)/bin/memscope
clean:
	rm -f memscope test_proc test_support test_cli
