SDK     := ../orion-sdk
COMM_I  := $(SDK)/Communications
UTIL_I  := $(SDK)/Utils
COMM_L  := $(SDK)/Communications/x86
UTIL_L  := $(SDK)/Utils/x86

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c99 -D_GNU_SOURCE
CPPFLAGS = -I$(COMM_I) -I$(UTIL_I)
LDFLAGS  = -L$(COMM_L) -L$(UTIL_L)
LDLIBS   = -Wl,--start-group -lOrionComm -lOrionUtils -Wl,--end-group -lm -lpthread

SRC     := $(wildcard src/*.c)
OBJ     := $(SRC:.c=.o)
BIN     := orionctl

.PHONY: all clean test

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(OBJ) -o $@ $(LDFLAGS) $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

tests/test_fov_math: tests/test_fov_math.c src/fov_math.c src/fov_math.h
	$(CC) $(CFLAGS) tests/test_fov_math.c src/fov_math.c -o $@ -lm

test: tests/test_fov_math
	@./tests/test_fov_math

clean:
	rm -f $(OBJ) $(BIN) tests/*.o tests/test_*
