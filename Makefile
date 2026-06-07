# blackwell-vram-temp — rootless RTX 50 (Blackwell) GDDR7 VRAM temperature reader
#
#   make           build ./blackwell-vram-temp
#   make install   copy the binary into ~/.local/bin
#   make clean     remove the built binary and object files

CC      ?= gcc
CFLAGS  ?= -O2 -Wall
SRC      = blackwell_vram_temp.c
BIN      = blackwell-vram-temp
PREFIX  ?= $(HOME)/.local
BINDIR   = $(PREFIX)/bin

.PHONY: all install clean

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(BIN)

install: $(BIN)
	mkdir -p $(BINDIR)
	cp $(BIN) $(BINDIR)/$(BIN)
	@echo "Installed $(BIN) -> $(BINDIR)/$(BIN)"
	@echo "Ensure $(BINDIR) is on your PATH."

clean:
	rm -f $(BIN) *.o
