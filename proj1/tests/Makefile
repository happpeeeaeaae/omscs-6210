CC ?= cc
CPPFLAGS += -Imock_libvirt
CFLAGS := -std=gnu11 -g -Wall -Werror $(CFLAGS)
COMMON = mock.h mock_libvirt/libvirt/libvirt.h
SANITIZERS = -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: check sanitize

check: .build/cpu_test .build/memory_test
	./.build/cpu_test
	./.build/memory_test

.build:
	mkdir -p $@

.build/cpu_test: cpu_test.c $(COMMON) ../cpu/src/vcpu_scheduler.c | .build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ -lm

.build/memory_test: memory_test.c $(COMMON) ../memory/src/memory_coordinator.c | .build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

.build/cpu_test_sanitize: cpu_test.c $(COMMON) ../cpu/src/vcpu_scheduler.c | .build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) $< -o $@ -lm

.build/memory_test_sanitize: memory_test.c $(COMMON) ../memory/src/memory_coordinator.c | .build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) $< -o $@

# LeakSanitizer cannot run under the sandbox's ptrace wrapper. ASan/UBSan can.
sanitize: .build/cpu_test_sanitize .build/memory_test_sanitize
	ASAN_OPTIONS="$${ASAN_OPTIONS:-detect_leaks=0}" UBSAN_OPTIONS=halt_on_error=1 ./.build/cpu_test_sanitize
	ASAN_OPTIONS="$${ASAN_OPTIONS:-detect_leaks=0}" UBSAN_OPTIONS=halt_on_error=1 ./.build/memory_test_sanitize
