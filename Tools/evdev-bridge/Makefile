CC = gcc
CFLAGS = -O2 -Wall
LDFLAGS = -lwayland-client -lxkbcommon

PROTO_DIR = protocols
PROTO_SRCS = $(PROTO_DIR)/wlr-virtual-pointer-unstable-v1-protocol.c \
             $(PROTO_DIR)/virtual-keyboard-unstable-v1-protocol.c

TARGET = evdev-bridge

all: $(TARGET)

$(TARGET): bridge.c $(PROTO_SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)/usr/local/bin/$(TARGET)

clean:
	rm -f $(TARGET)

# Regenerate protocol files (requires wayland-scanner, wlr-protocols)
protocols:
	wayland-scanner client-header /usr/share/wlr-protocols/unstable/wlr-virtual-pointer-unstable-v1.xml $(PROTO_DIR)/wlr-virtual-pointer-unstable-v1-client-protocol.h
	wayland-scanner private-code /usr/share/wlr-protocols/unstable/wlr-virtual-pointer-unstable-v1.xml $(PROTO_DIR)/wlr-virtual-pointer-unstable-v1-protocol.c
	curl -sL https://raw.githubusercontent.com/swaywm/wlroots/master/protocol/virtual-keyboard-unstable-v1.xml -o /tmp/vk.xml
	wayland-scanner client-header /tmp/vk.xml $(PROTO_DIR)/virtual-keyboard-unstable-v1-client-protocol.h
	wayland-scanner private-code /tmp/vk.xml $(PROTO_DIR)/virtual-keyboard-unstable-v1-protocol.c

.PHONY: all install clean protocols
