# Rules for GoFish gopher server

sbin_PROGRAMS = gopherd
gopherd_SOURCES = gopherd.c log.c socket.c config.c http.c mmap_cache.c

EXTRA_DIST = COPYING README INSTALL NEWS AUTHORS ChangeLog \
	init-gofish gofish.spec

man_MANS = gofish.1 gofish.5 dotcache.5 gopherd.1 mkcache.1 gmap2cache.1

rootdir=$(localstatedir)/lib/gopherd
icondir=$(rootdir)/icons

# Do not override the gofish.conf or icons directories if they exist
install-data-local: install-root-dir
	$(mkinstalldirs) $(DESTDIR)$(sysconfdir)
	if test -f $(DESTDIR)$(sysconfdir)/gofish.conf; then \
	  $(INSTALL_DATA) gofish.conf $(DESTDIR)$(sysconfdir)/gofish.conf.new; \
	else \
	  $(INSTALL_DATA) gofish.conf $(DESTDIR)$(sysconfdir)/gofish.conf; \
	fi

# 1. Only install Configure_GoFish if /var/lib/gopherd does not exist.
#    We don't want to keep installing it for users who are upgrading.
# 2. Only install the .gopher+ file if it dosen't exist. We don't want to
#    override any user changes.
# 3. Only install icon directory if it dosen't exist. We don't want to
#    override any user installed icons.
install-root-dir:
	if test ! -d $(DESTDIR)$(rootdir); then \
	  $(mkinstalldirs) $(DESTDIR)$(rootdir); \
	  $(INSTALL_DATA) Configure_GoFish $(DESTDIR)$(rootdir)/Configure_GoFish; \
	  $(INSTALL_DATA) _cache $(DESTDIR)$(rootdir)/.cache; \
	fi
	if test ! -f $(DESTDIR)$(rootdir)/.gopher+; then \
	  $(INSTALL_DATA) _gopher+ $(DESTDIR)$(rootdir)/.gopher+; \
	fi
	if test ! -f $(DESTDIR)$(icondir); then \
	  $(mkinstalldirs) $(DESTDIR)$(icondir); \
	  $(INSTALL_DATA) $(srcdir)/icons/*.gif $(DESTDIR)$(icondir); \
	fi

# Do not remove the root_dir or any part of it
uninstall-local:
	rm -f  $(DESTDIR)$(sysconfdir)/gofish.conf
	rm -f  $(DESTDIR)$(sysconfdir)/gofish.conf.new

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
