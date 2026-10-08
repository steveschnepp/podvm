# podvm guest init
# Build: musl-gcc -static -Os (falls back to gcc when musl is absent)

MUSL := $(shell command -v musl-gcc 2>/dev/null)
CC = $(if $(MUSL),musl-gcc,gcc)
CFLAGS = -static -Os -fno-stack-protector -fno-asynchronous-unwind-tables \
         -ffunction-sections -fdata-sections
LDFLAGS = -s -Wl,--gc-sections

SRC = init.c ../src/strparse.c ../src/userdb.c
INC = -I../src
OUT = init

all: $(OUT)

$(OUT): $(SRC)
	$(CC) $(CFLAGS) $(INC) $(LDFLAGS) -o $@ $^
	@size $@

clean:
	rm -f $(OUT)

.PHONY: all clean
