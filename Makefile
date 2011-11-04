bin_PROGRAMS = reset script scriptreplay
man1_PAGES = reset.1 script.1 scriptreplay.1
CC = gcc -std=gnu99
CPPFLAGS =
CFLAGS = -g -O2
INSTALL = ginstall

all: $(bin_PROGRAMS)

clean:
	$(RM) $(bin_PROGRAMS)

script: script.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^

scriptreplay: scriptreplay.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^

install: all
	$(INSTALL) -m 755 -d $(DESTDIR)/bin/
	$(INSTALL) -m 755 $(bin_PROGRAMS) $(DESTDIR)/bin/
	$(INSTALL) -m 755 -d $(DESTDIR)/share/man/man1/
	$(INSTALL) -m 644 $(man1_PAGES) $(DESTDIR)/share/man/man1/

.PHONY: all clean install
