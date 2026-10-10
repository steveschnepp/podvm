# podvm guest init
# Build: musl-gcc -static -Os (falls back to gcc when musl is absent)

MUSL := $(shell command -v musl-gcc 2>/dev/null)
CC = $(if $(MUSL),musl-gcc,gcc)
# musl-gcc deliberately drops /usr/include (glibc headers would
# contaminate a musl static build), but the kernel UAPI headers
# (linux/, asm/) are self-contained. -idirafter appends /usr/include
# AFTER all standard dirs, so musl's own headers still win and only
# what musl lacks (the UAPI headers) comes from there.
MUSL_UAPI = $(if $(MUSL),-idirafter /usr/include)
CFLAGS = -static -Os -fno-stack-protector -fno-asynchronous-unwind-tables \
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
