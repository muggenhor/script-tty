bin_PROGRAMS = script scriptreplay
man1_PAGES = reset.1 script.1 scriptreplay.1
CC = gcc -std=gnu99
CPPFLAGS =
CFLAGS = -g -O2 -Wall
INSTALL = ginstall

all: $(bin_PROGRAMS)

clean:
	$(RM) $(bin_PROGRAMS)

script: script.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

scriptreplay: scriptreplay.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

install-bin: $(bin_PROGRAMS) reset
	$(INSTALL) -m 755 -d $(DESTDIR)$(PREFIX)/bin/
	$(INSTALL) -m 755 $^ $(DESTDIR)$(PREFIX)/bin/

install-man: $(man1_PAGES)
	$(INSTALL) -m 755 -d $(DESTDIR)$(PREFIX)/share/man/man1/
	$(INSTALL) -m 644 $^ $(DESTDIR)$(PREFIX)/share/man/man1/

install: install-bin install-man

.PHONY: all clean install install-bin install-man
