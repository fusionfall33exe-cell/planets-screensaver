# planets - the solar system in your terminal
# Build with:    make
# Run with:      make run        (or ./planets, see ./planets -h)
# Install with:  make install    (global command "Planets" in ~/.local/bin)

CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -O2 -pthread
LDLIBS  := -lm
TARGET  := planets
NAME    := Planets
PREFIX  ?= $(HOME)/.local

# Tests: sanitizer builds of planets and the test programs in tests/
TESTFLAGS := -std=c11 -O1 -g -pthread -Wall -Wextra
SANITIZE  := -fsanitize=address,undefined -fno-sanitize-recover=undefined
TESTBINS  := tests/render_test tests/output_test tests/planets_san tests/render_tsan

.PHONY: build run install uninstall clean test test-memory test-threads

build: $(TARGET)

$(TARGET): planets.c
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

run: $(TARGET)
	./$(TARGET)

# Linux commands are case-sensitive, so also add a lowercase "planets".
install: $(TARGET)
	install -Dm755 $(TARGET) $(PREFIX)/bin/$(NAME)
	ln -sf $(NAME) $(PREFIX)/bin/planets

uninstall:
	rm -f $(PREFIX)/bin/$(NAME) $(PREFIX)/bin/planets

# make test: everything. test-memory: AddressSanitizer + UBSan. test-threads: ThreadSanitizer.
test: test-memory test-threads

test-memory: tests/render_test tests/output_test tests/planets_san
	./tests/render_test
	./tests/output_test
	bash tests/run_tests.sh tests/planets_san

# setarch -R: ThreadSanitizer cannot cope with the address randomization of newer kernels
test-threads: tests/render_tsan
	TSAN_OPTIONS=halt_on_error=1 setarch $$(uname -m) -R ./tests/render_tsan

tests/%_test: tests/%_test.c planets.c
	$(CC) $(TESTFLAGS) $(SANITIZE) $< -o $@ $(LDLIBS)

tests/planets_san: planets.c
	$(CC) $(TESTFLAGS) $(SANITIZE) $< -o $@ $(LDLIBS)

tests/render_tsan: tests/render_test.c planets.c
	$(CC) $(TESTFLAGS) -fsanitize=thread $< -o $@ $(LDLIBS)

clean:
	rm -f $(TARGET) $(TESTBINS)
