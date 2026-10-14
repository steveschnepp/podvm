# podvm guest init
# Build: musl-gcc -static -Os (falls back to gcc when musl is absent)
#
# The initramfs must stay portable across both libcs, so the build is
# warning-clean under each (a glibc-only function shows up as an implicit
# declaration under -Wall). Force a specific one with:
#   make LIBC=musl     # musl-gcc (errors if musl is absent)
#   make LIBC=glibc    # gcc/glibc
#   make LIBC=auto     # musl if present, else glibc (default)
# tests/run_tests.sh builds BOTH ways so neither toolchain rots.

LIBC ?= auto
MUSL_FOUND := $(shell command -v musl-gcc 2>/dev/null)

ifeq ($(LIBC),musl)
  ifeq ($(MUSL_FOUND),)
    $(error LIBC=musl requested but musl-gcc was not found)
  endif
  CC := musl-gcc
else ifeq ($(LIBC),glibc)
  CC := gcc
else ifeq ($(LIBC),auto)
  CC := $(if $(MUSL_FOUND),musl-gcc,gcc)
else
  $(error LIBC must be musl, glibc, or auto (got '$(LIBC)'))
endif

# musl-gcc deliberately drops /usr/include (glibc headers would
# contaminate a musl static build), but the kernel UAPI headers
# (linux/, asm/) are self-contained. -idirafter appends these dirs AFTER
# all standard dirs, so musl's own headers still win and only what musl
# lacks (the UAPI headers) comes from there.
#
# /usr/include alone is NOT enough: on Debian/Ubuntu the arch-specific
# UAPI headers (asm/types.h, pulled in by linux/vm_sockets.h) live in the
# multiarch dir /usr/include/<triplet>, which plain gcc searches
# automatically but musl-gcc does not. Hosts without the
# /usr/include/asm -> multiarch symlink (a libc6-dev artifact) would
# otherwise fail to build with "asm/types.h: No such file". Add the
# multiarch dir too, detected so it is portable across architectures.
# glibc gcc needs no such hint.
MULTIARCH := $(shell gcc -print-multiarch 2>/dev/null)
MUSL_UAPI = $(if $(filter musl-gcc,$(CC)),-idirafter /usr/include \
              $(if $(MULTIARCH),-idirafter /usr/include/$(MULTIARCH)))
CFLAGS = -static -Os -Wall -Wextra -fno-stack-protector \
         -fno-asynchronous-unwind-tables \
         -ffunction-sections -fdata-sections
LDFLAGS = -s -Wl,--gc-sections

SRC = init.c ../src/strparse.c ../src/userdb.c
INC = -I../src $(MUSL_UAPI)
OUT = init

all: $(OUT)

$(OUT): $(SRC)
	$(CC) $(CFLAGS) $(INC) $(LDFLAGS) -o $@ $^
	@size $@

clean:
	rm -f $(OUT)

.PHONY: all clean
