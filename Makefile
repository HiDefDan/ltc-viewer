CC ?= gcc
CFLAGS = -std=c99 -Wall -Wextra -O2 -I./include $(shell pkg-config --cflags libdrm libpng)
LDFLAGS = $(shell pkg-config --libs libdrm libpng)

# Raspberry Pi 5 native build
# Build dependencies: sudo apt install -y build-essential libdrm-dev libpng-dev pkg-config

# Source and object files
SRCS = src/main.c src/drm.c src/font.c src/ltc.c src/gpio.c
OBJS = $(SRCS:.c=.o)
TARGET = ltc-timecode

# Default target
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "[BUILD] $@ complete"

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/
	install -d /usr/share/ltc-timecode/glyphs
	install -m 644 data/glyphs/*.png /usr/share/ltc-timecode/glyphs/
	install -m 644 systemd/ltc-timecode.service /etc/systemd/system/
	systemctl daemon-reload
	@echo "[INSTALL] Installed. Enable with: systemctl enable ltc-timecode"

uninstall:
	systemctl stop ltc-timecode || true
	systemctl disable ltc-timecode || true
	rm -f /usr/local/bin/$(TARGET)
	rm -rf /usr/share/ltc-timecode/glyphs
	rm -f /etc/systemd/system/ltc-timecode.service
	systemctl daemon-reload

start:
	systemctl start ltc-timecode

stop:
	systemctl stop ltc-timecode

status:
	systemctl status ltc-timecode

logs:
	journalctl -u ltc-timecode -f

.PHONY: all clean install uninstall start stop status logs
