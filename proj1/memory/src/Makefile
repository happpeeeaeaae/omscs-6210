CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
LDLIBS += -lvirt

.PHONY: all clean

all: memory_coordinator

memory_coordinator: memory_coordinator.c
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

clean:
	rm -f memory_coordinator
