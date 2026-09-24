# SPDX-License-Identifier: GPL-3.0-or-later

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
LDFLAGS ?=

TARGET  := pibrick-button-service
SOURCE  := pibrick-button-service.c
SERVICE := pibrick-button-service.service

PREFIX   ?= /usr
SBINDIR  ?= $(PREFIX)/sbin
SYSCONFDIR ?= /etc
SYSTEMD_SYSTEM_UNIT_DIR ?= /etc/systemd/system

CONFIG_SOURCE_DIR := etc/pibrick
CONFIG_DIR        := $(SYSCONFDIR)/pibrick

.PHONY: all clean install install-config uninstall uninstall-config

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)

install: $(TARGET) install-config
	install -D -m 0755 $(TARGET) \
		$(DESTDIR)$(SBINDIR)/$(TARGET)

	install -D -m 0644 $(SERVICE) \
		$(DESTDIR)$(SYSTEMD_SYSTEM_UNIT_DIR)/$(SERVICE)

	systemctl daemon-reload
	systemctl enable $(SERVICE)
	systemctl restart $(SERVICE) || systemctl start $(SERVICE)

install-config:
	install -d -m 0755 $(DESTDIR)$(CONFIG_DIR)
	cp -a $(CONFIG_SOURCE_DIR)/. $(DESTDIR)$(CONFIG_DIR)/

	# Ensure all action/config scripts remain executable.
	find $(DESTDIR)$(CONFIG_DIR) -type f -name '*.sh' \
		-exec chmod 0755 {} \;

uninstall:
	systemctl disable --now $(SERVICE) 2>/dev/null || true

	rm -f $(DESTDIR)$(SBINDIR)/$(TARGET)
	rm -f $(DESTDIR)$(SYSTEMD_SYSTEM_UNIT_DIR)/$(SERVICE)

	rm -rf $(DESTDIR)$(CONFIG_DIR)

	systemctl daemon-reload

uninstall-config:
	rm -rf $(DESTDIR)$(CONFIG_DIR)
