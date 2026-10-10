# blackwell-vram-temp — rootless RTX 50 (Blackwell) GDDR7 VRAM temperature reader
#
#   make           build ./blackwell-vram-temp
#   make install   copy the binary into ~/.local/bin
#   make install-cap  install, then grant CAP_SYS_ADMIN via sudo setcap
#                     (needed for the driver 615+ register fallback)
#   make clean     remove the built binary and object files

CC      ?= gcc
CFLAGS  ?= -O2 -Wall
SRC      = blackwell_vram_temp.c
BIN      = blackwell-vram-temp
PREFIX  ?= $(HOME)/.local
BINDIR   = $(PREFIX)/bin

.PHONY: all install install-cap clean

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(BIN)

install: $(BIN)
	mkdir -p $(BINDIR)
	cp $(BIN) $(BINDIR)/$(BIN)
	@echo "Installed $(BIN) -> $(BINDIR)/$(BIN)"
	@echo "Ensure $(BINDIR) is on your PATH."

# Copying a new binary over the old one drops its file capability, so the
# grant has to be reapplied after every install.
install-cap: install
	sudo setcap cap_sys_admin+ep $(BINDIR)/$(BIN)
	@getcap $(BINDIR)/$(BIN)

clean:
	rm -f $(BIN) *.o
