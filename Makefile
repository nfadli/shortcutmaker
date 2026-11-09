CC = gcc
CFLAGS = -Wall -O2 $(shell pkg-config --cflags gtk+-3.0)
LIBS = $(shell pkg-config --libs gtk+-3.0)

TARGET = shortcutmaker
SRC = shortcutmaker.c
PREFIX ?= $(HOME)/.local

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(SRC) -o $(TARGET) $(CFLAGS) $(LIBS)

install: $(TARGET)
	mkdir -p $(PREFIX)/bin
	cp $(TARGET) $(PREFIX)/bin/
	chmod +x $(PREFIX)/bin/$(TARGET)
	
	mkdir -p $(PREFIX)/share/applications
	cp shortcutmaker.desktop $(PREFIX)/share/applications/
	
	@echo "Instalasi selesai! shortcutmakersiap digunakan."

clean:
	rm -f $(TARGET)

.PHONY: all install clean
