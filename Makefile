CFLAGS += -Wall -O6
# For valgrind
#CFLAGS += -Wall -O -g

# This can be overridden
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
SBINDIR ?= $(PREFIX)/sbin
MANDIR ?= $(PREFIX)/man
SYSCONFDIR ?= /etc

OBJS:=	gopherd.o log.o socket.o config.o

# For the sendfile(2) call
CFLAGS += -DUSE_SENDFILE

# For the mmap(2) call
CFLAGS += -DUSE_MMAP

# For the http gateway
CFLAGS += -DUSE_HTTP
OBJS   += http.o


all: gopherd mkcache

gopherd: $(OBJS)

$(OBJS): gopherd.h

gopherd.c: version.h

version.h: gofish.spec
	@echo "version=`fgrep Version: gofish.spec | cut -d' ' -f2`" > v.sh
	@echo 'echo "#define GOFISH_VERSION \"$$version\"" > version.h' >> v.sh
	@sh v.sh
	@rm v.sh
	@echo "Updated version.h"

mkcache: mkcache.o config.o

catfish: catfish.o socket.o
	$(CC) $(CFLAGS) -o catfish catfish.o socket.o

install:
	install -D -m 700 -s gopherd  $(SBINDIR)/gopherd
	install -D -m 755 -s mkcache  $(BINDIR)/mkcache
	install -D -m 755 check-files $(BINDIR)/check-files
	install -D -m 644 gofish.1    $(MANDIR)/man1/gofish.1
	install -D -m 644 gofish.5    $(MANDIR)/man5/gofish.5
	install -D -m 644 dotcache.5  $(MANDIR)/man5/dotcache.5
	install -D -m 644 gopherd.1   $(MANDIR)/man1/gopherd.1
	install -D -m 644 mkcache.1   $(MANDIR)/man1/mkcache.1
	install -D -m 644 gofish.conf $(SYSCONFDIR)/gofish.conf

clean:
	rm -f *.o version.h gopherd mkcache catfish core *~ TAGS
