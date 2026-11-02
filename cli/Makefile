# SPDX-License-Identifier: GPL-2.0
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic
CPPFLAGS += -I../include

TARGET := alg-rgb

all: $(TARGET)

$(TARGET): alg-rgb.c ../include/version.h ../include/protocol.h
	$(CC) $(CPPFLAGS) $(CFLAGS) alg-rgb.c -o $(TARGET) -lm

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)/usr/local/bin/$(TARGET)

.PHONY: all clean install
