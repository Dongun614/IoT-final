CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -I/usr/local/include
LIBS    = -L/usr/local/lib -lmosquitto

TARGET  = lora_bridge
SRC     = lora_bridge.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
