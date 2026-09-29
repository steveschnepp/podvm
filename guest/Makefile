# podvm guest init
# Build: musl-gcc -static -Os

CC = musl-gcc
CFLAGS = -static -Os -fno-stack-protector -fno-asynchronous-unwind-tables \
         -ffunction-sections -fdata-sections
LDFLAGS = -s -Wl,--gc-sections

SRC = init.c
OUT = init

all: $(OUT)

$(OUT): $(SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^
	@size $@

clean:
	rm -f $(OUT)

.PHONY: all clean
