# Rules for GoFish gopher server

sbin_PROGRAMS = gopherd
gopherd_SOURCES = gopherd.c log.c socket.c config.c http.c

EXTRA_DIST = COPYING README INSTALL NEWS AUTHORS ChangeLog \
	init-gofish gofish.spec

man_MANS = gofish.1 gofish.5 dotcache.5 gopherd.1 mkcache.1 gmap2cache.1

rootdir=$(localstatedir)/lib/gopherd
icondir=$(rootdir)/icons
root_DATA = .gopher+

# Do not override the gofish.conf or icons directories if they exist
install-data-local:
	$(mkinstalldirs) $(DESTDIR)$(sysconfdir)
	if test -f $(DESTDIR)$(sysconfdir)/gofish.conf; then \
	  $(INSTALL_DATA) gofish.conf $(DESTDIR)$(sysconfdir)/gofish.conf.new; \
	else \
	  $(INSTALL_DATA) gofish.conf $(DESTDIR)$(sysconfdir)/gofish.conf; \
	fi
	if test ! -f $(DESTDIR)$(icondir); then \
		$(mkinstalldirs) $(DESTDIR)$(icondir); \
		$(INSTALL_DATA) $(srcdir)/icons/*.gif $(DESTDIR)$(icondir); \
	fi

uninstall-local:
	rm -f  $(DESTDIR)$(sysconfdir)/gofish.conf
	rm -f  $(DESTDIR)$(sysconfdir)/gofish.conf.new
	rm -rf $(DESTDIR)$(icondir)
	rmdir  $(DESTDIR)$(rootdir)

.gopher+: _gopher+
	cp _gopher+ .gopher+

# Create version.h from gofish.spec
# Don't use BUILT_SOURCES
gopherd.o http.o: version.h

version.h: gofish.spec
	@version=`fgrep Version: gofish.spec | cut -d' ' -f2`; \
	echo "#define GOFISH_VERSION \"$$version\"" > version.h; \
	echo "Updated version.h to $$version"

# Extra helper programs

bin_PROGRAMS = mkcache gmap2cache
mkcache_SOURCES = mkcache.c config.c
gmap2cache_SOURCES=gmap2cache.c config.c
bin_SCRIPTS = check-files
