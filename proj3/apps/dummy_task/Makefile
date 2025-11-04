# Cross-compile for 32-bit Raspberry Pi
CROSS_COMPILE ?= arm-linux-gnueabihf-

CC      := $(CROSS_COMPILE)gcc
CFLAGS  := -O2 -Wall -Wextra -Werror
LDFLAGS :=

TARGET  := dummy_task
SRC     := dummy_task.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
