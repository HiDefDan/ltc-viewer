CC ?= gcc

# Common compile flags
BASE_CFLAGS = -std=c99 -Wall -Wextra -O2 -I./include

# Package groups (queried individually so one missing .pc doesn't hide all others)
MAIN_PKGS = libdrm libpng ltc alsa
CONFIG_PKGS = libdrm libpng libmicrohttpd libwebsockets

# Resolve pkg-config flags package-by-package to avoid all-or-nothing failures
MAIN_CFLAGS = $(BASE_CFLAGS) \
	$(foreach p,$(MAIN_PKGS),$(shell pkg-config --cflags $(p) 2>/dev/null))
MAIN_LDFLAGS = $(foreach p,$(MAIN_PKGS),$(shell pkg-config --libs $(p) 2>/dev/null)) -pthread

CONFIG_CFLAGS = $(BASE_CFLAGS) \
	$(foreach p,$(CONFIG_PKGS),$(shell pkg-config --cflags $(p) 2>/dev/null))
CONFIG_LDFLAGS = $(foreach p,$(CONFIG_PKGS),$(shell pkg-config --libs $(p) 2>/dev/null)) -pthread

# Compile-time include flags must cover all source files
COMPILE_CFLAGS = $(BASE_CFLAGS) \
	$(foreach p,$(MAIN_PKGS) $(CONFIG_PKGS),$(shell pkg-config --cflags $(p) 2>/dev/null))

# Raspberry Pi CM5 native build
# Build dependencies: sudo apt install -y git pkg-config build-essential xxd libdrm-dev libpng-dev libmicrohttpd-dev libwebsockets-dev libltc-dev libasound2-dev

# Main timecode application source files
MAIN_SRCS = src/main.c src/drm.c src/font.c src/ltc.c src/gpio.c src/config-management.c src/config-watcher.c
MAIN_OBJS = $(MAIN_SRCS:.c=.o)
MAIN_TARGET = ltc-timecode

# Config daemon application source files
CONFIG_SRCS = src/config-daemon.c src/config-management.c src/config-service.c src/web-embedded.c
CONFIG_OBJS = $(CONFIG_SRCS:.c=.o)
CONFIG_TARGET = ltc-config-daemon

# Web embedding
WEB_EMBEDDED = src/web-embedded.c

# Default target
all: $(MAIN_TARGET) $(CONFIG_TARGET)

$(MAIN_TARGET): $(MAIN_OBJS)
	$(CC) $(MAIN_CFLAGS) -o $@ $^ $(MAIN_LDFLAGS)
	@echo "[BUILD] $@ complete"

$(CONFIG_TARGET): $(WEB_EMBEDDED) $(CONFIG_OBJS)
	$(CC) $(CONFIG_CFLAGS) -o $@ $(CONFIG_OBJS) $(CONFIG_LDFLAGS)
	@echo "[BUILD] $@ complete"

$(WEB_EMBEDDED): web/index.html web/styles.css web/app.js
	@echo "[EMBED] Embedding web files..."
	@bash scripts/embed-web.sh $@ .

%.o: %.c
	$(CC) $(COMPILE_CFLAGS) -c $< -o $@

clean:
	rm -f $(MAIN_OBJS) $(MAIN_TARGET) $(CONFIG_OBJS) $(CONFIG_TARGET) $(WEB_EMBEDDED)

install: all
	install -d /etc/ltc-viewer
	install -m 755 $(MAIN_TARGET) /usr/local/bin/
	install -m 755 $(CONFIG_TARGET) /usr/local/bin/
	install -d /usr/share/ltc-timecode/glyphs
	install -m 644 data/glyphs/*.png /usr/share/ltc-timecode/glyphs/
	install -m 644 systemd/ltc-timecode.service /etc/systemd/system/
	install -m 644 systemd/ltc-config.service /etc/systemd/system/
	install -m 644 config/default-config.json /etc/ltc-viewer/config.json.default
	@if [ ! -f /etc/ltc-viewer/config.json ]; then \
	    cp config/default-config.json /etc/ltc-viewer/config.json; \
	    chmod 644 /etc/ltc-viewer/config.json; \
	fi
	install -d /etc/sudoers.d
	install -m 440 config/sudoers.d/ltc-config /etc/sudoers.d/ltc-config
	systemctl daemon-reload
	@echo "[INSTALL] Installed. Enable with: systemctl enable ltc-config ltc-timecode"

uninstall:
	systemctl stop ltc-timecode ltc-config || true
	systemctl disable ltc-timecode ltc-config || true
	rm -f /usr/local/bin/$(MAIN_TARGET)
	rm -f /usr/local/bin/$(CONFIG_TARGET)
	rm -rf /usr/share/ltc-timecode/glyphs
	rm -f /etc/systemd/system/ltc-timecode.service
	rm -f /etc/systemd/system/ltc-config.service
	rm -f /etc/sudoers.d/ltc-config
	systemctl daemon-reload

start:
	systemctl start ltc-config ltc-timecode

stop:
	systemctl stop ltc-timecode ltc-config

status:
	systemctl status ltc-config
	systemctl status ltc-timecode

logs:
	journalctl -u ltc-timecode -u ltc-config -f

.PHONY: all clean install uninstall start stop status logs
