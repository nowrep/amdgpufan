CC = gcc
CFLAGS += -Wall -Wextra -pedantic

TARGET = amdgpufan
VERSION = 0.1

ifeq ($(BUILD),debug)
CFLAGS += -O0 -g
else
CFLAGS += -O2 -s -DNDEBUG
endif

DEFINES += -DAMDGPUFAN_VERSION=\"$(VERSION)\"

all:
	$(CC) main.c -o $(TARGET) $(DEFINES) $(CFLAGS) $(LIBS)

debug:
	make "BUILD=debug"

install: all
	install -D -m 755 -p $(TARGET) $(DESTDIR)/usr/bin/$(TARGET)

uninstall:
	rm -f $(DESTDIR)/usr/bin/$(TARGET)
