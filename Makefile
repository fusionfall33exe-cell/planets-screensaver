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

.PHONY: build run install uninstall clean

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

clean:
	rm -f $(TARGET)
