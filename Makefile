# ─────────────────────────────────────────────────────────────────
#  Makefile — KB Analytics (kernel module + user-space app)
#
#  Targets:
#    make            → build both the kernel module and user-space app
#    make module     → build only the .ko
#    make userspace  → build only the user-space binary
#    make clean      → remove all build artefacts
# ─────────────────────────────────────────────────────────────────

# Kernel build directory — uses currently running kernel by default
KDIR   ?= /lib/modules/$(shell uname -r)/build
PWD    := $(shell pwd)

# User-space compiler settings
CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=c11

# Module object
obj-m   += kb_module.o

# ── Default target ──────────────────────────────────────────────
.PHONY: all
all: module userspace

# ── Kernel module ───────────────────────────────────────────────
.PHONY: module
module:
	@echo ">>> Building kernel module kb_module.ko ..."
	$(MAKE) -C $(KDIR) M=$(PWD) modules
	@echo ">>> Kernel module built successfully."

# ── User-space application ──────────────────────────────────────
.PHONY: userspace
userspace: userspace_app mmap_reader
	@echo ">>> User-space binaries built successfully."

userspace_app: userspace_app.c
	@echo ">>> Compiling userspace_app.c with gcc ..."
	$(CC) $(CFLAGS) -o $@ $<

mmap_reader: mmap_reader.c
	@echo ">>> Compiling mmap_reader.c with gcc ..."
	$(CC) $(CFLAGS) -o $@ $<

# ── Clean ───────────────────────────────────────────────────────
.PHONY: clean
clean:
	@echo ">>> Cleaning build artefacts ..."
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f userspace_app mmap_reader
	@echo ">>> Clean done."