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
check: memscope test_proc
	./test_proc
	python3 tests.py ./memscope
install: memscope
	install -Dm755 memscope $(DESTDIR)$(PREFIX)/bin/memscope
clean:
	rm -f memscope test_proc
