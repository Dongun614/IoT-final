CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -pthread
TARGET  = robot
SRCS    = main.c fsm.c
OBJS    = $(SRCS:.c=.o)

include ../ports.env
export

.PHONY: all run test-mock clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

run: $(TARGET)
	./$(TARGET) --uart $(ROBOT_PORT) --server-ip $(SERVER_IP) --server-port $(WIFI_PORT)

test-mock: $(TARGET)
	./$(TARGET) --uart /dev/null --mock-rssi -50 --fast-test

clean:
	rm -f $(OBJS) $(TARGET)
