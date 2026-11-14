CC = clang

# Optimisasi Ekstrem untuk Clang & LLVM
CFLAGS = -Wall -Wextra -O3 -march=native -mtune=native \
         -flto=full -fno-plt -fomit-frame-pointer \
         -funroll-loops -ffast-math \
         -fno-stack-protector \
         $(shell pkg-config --cflags gtk+-3.0)

LDFLAGS = -fuse-ld=lld -flto=full -Wl,-O3 -Wl,--as-needed \
          $(shell pkg-config --libs gtk+-3.0)

TARGET = shortcutmaker
SRC = shortcutmaker.c
PREFIX ?= $(HOME)/.local

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(SRC) -o $(TARGET) $(CFLAGS) $(LDFLAGS)
	# Membuang seluruh debug symbol untuk memperkecil ukuran & mempercepat pemuatan binary
	strip --strip-all $(TARGET)

install: $(TARGET)
	mkdir -p $(PREFIX)/bin
	cp $(TARGET) $(PREFIX)/bin/
	chmod +x $(PREFIX)/bin/$(TARGET)
	
	mkdir -p $(PREFIX)/share/applications
	cp shortcutmaker.desktop $(PREFIX)/share/applications/
	
	@echo "Instalasi selesai! shortcutmaker siap digunakan."

clean:
	rm -f $(TARGET)

.PHONY: all install clean
