#usb makefile

CC = arm-linux-gnueabihf-gcc
LD = arm-linux-gnueabihf-gcc
CFLAGS = -Wall -g -c 


SRCS = $(wildcard *.c)
OBJS = $(patsubst %c,%o,$(SRCS))
TARGET = cycle_buffer

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(LD) -o $@ $^ -pthread
%o: %c
	$(CC) $(CFLAGS) -o $@  $<  
	
clean:
	@rm -f *.o $(TARGET)
