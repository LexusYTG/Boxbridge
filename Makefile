# Boxbridge - interceptor x86 -> box64 via LD_PRELOAD
CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -fPIC
LDFLAGS ?= -shared
LDLIBS  ?= -ldl -pthread

libboxbridge.so: boxbridge.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

# En container glibc:
#   make
# En Termux bionic (aunque no aplica, no hay box64 bionic):
#   make CC=clang

clean:
	rm -f libboxbridge.so

.PHONY: clean
