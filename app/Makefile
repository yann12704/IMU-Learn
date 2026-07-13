CROSS_COMPILE ?= arm-linux-gnueabihf-
CC := $(CROSS_COMPILE)gcc

CPPFLAGS += -I../include
CFLAGS ?= -O2 -g
override CFLAGS += -std=c11 -Wall -Wextra -Werror
LDLIBS += -lm

TARGET := attitude_app
OBJECTS := attitude_app.o attitude_filter.o

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

attitude_app.o: attitude_app.c attitude_filter.h ../include/imu_sample.h
attitude_filter.o: attitude_filter.c attitude_filter.h

clean:
	$(RM) $(TARGET) $(OBJECTS)
