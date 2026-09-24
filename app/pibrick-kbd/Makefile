# SPDX-License-Identifier: GPL-3.0-or-later

TARGET := pibrick-kbd.sh

PREFIX  ?= /usr
BINDIR  ?= $(PREFIX)/local/bin

.PHONY: all install uninstall clean

all: $(TARGET)

install: $(TARGET)
	install -D -m 0755 \
		$(TARGET) \
		$(DESTDIR)$(BINDIR)/pibrick-kbd

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/pibrick-kbd

clean:
	@: