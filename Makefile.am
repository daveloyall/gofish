# Rules for GoFish gopher server

sbin_PROGRAMS = gopherd
gopherd_SOURCES = gopherd.c log.c socket.c config.c http.c

EXTRA_DIST = COPYING README INSTALL NEWS AUTHORS ChangeLog \
	init-gofish gofish.spec

man_MANS = gofish.1 gofish.5 dotcache.5 gopherd.1 mkcache.1

sysconf_DATA = gofish.conf

rootdir=$(localstatedir)/lib/gopherd
icondir=$(rootdir)/icons
root_DATA = .gopher+

install-data-local:
	mkdir $(DESTDIR)$(icondir)
	$(INSTALL_DATA) $(srcdir)/icons/*.gif $(DESTDIR)$(icondir)

.gopher+: _gopher+
	cp _gopher+ .gopher+

# Create version.h from gofish.spec
# Don't use BUILT_SOURCES

gopherd.o http.o: version.h

version.h: gofish.spec
	@echo "version=`fgrep Version: gofish.spec | cut -d' ' -f2`" > v.sh
	@echo 'echo "#define GOFISH_VERSION \"$$version\"" > version.h' >> v.sh
	@sh v.sh
	@rm v.sh
	@echo "Updated version.h"

# Extra helper programs

bin_PROGRAMS = mkcache
mkcache_SOURCES = mkcache.c config.c
bin_SCRIPTS = check-files
