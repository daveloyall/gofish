Summary: A Gopher Server
Name: gofish
Version: 0.9
Release: 1
Copyright: GPL
Group: Networking/Daemons
Source:gopher://seanm.ca/9/winder/gofish-%{version}.tar.gz
BuildRoot: /var/tmp/%{name}-buildroot
Conflicts: gopher, gopherd

%description
GoFish is a very simple gopher server. It is designed with security
and low resource usage in mind. GoFish uses a single process that
handles all the connections. This provides low resource usage, good
latency (no context switches), and good scalability.

GoFish runs in a chroot(2) environment. This means that GoFish can
only serve files from the root directory or below. While GoFish must
run at root privilege to be able to use port 70, it drops to a normal
user while accessing files.

%prep
%setup

%build
make

%install
rm -rf $RPM_BUILD_ROOT

mkdir -p $RPM_BUILD_ROOT/usr/sbin
install -s -m 700 gopherd $RPM_BUILD_ROOT/usr/sbin/gopherd

mkdir -p $RPM_BUILD_ROOT/usr/bin
install -s -m 755 mkcache $RPM_BUILD_ROOT/usr/bin/mkcache
install -m 755 check-files $RPM_BUILD_ROOT/usr/bin/check-files

mkdir -p $RPM_BUILD_ROOT/usr/man/man1
mkdir -p $RPM_BUILD_ROOT/usr/man/man5
install -m 644 gofish.1 $RPM_BUILD_ROOT/usr/man/man1/gofish.1
install -m 644 gofish.5 $RPM_BUILD_ROOT/usr/man/man5/gofish.5
install -m 644 dotcache.5 $RPM_BUILD_ROOT/usr/man/man5/dotcache.5
install -m 644 gopherd.1 $RPM_BUILD_ROOT/usr/man/man1/gopherd.1
install -m 644 mkcache.1 $RPM_BUILD_ROOT/usr/man/man1/mkcache.1

mkdir -p $RPM_BUILD_ROOT/etc
install gofish.conf $RPM_BUILD_ROOT/etc/gofish.conf

mkdir -p $RPM_BUILD_ROOT/etc/rc.d/init.d
install init-gofish $RPM_BUILD_ROOT/etc/rc.d/init.d/gopherd

mkdir -p $RPM_BUILD_ROOT/usr/share/gofish/icons
install -m644 icons/*.gif $RPM_BUILD_ROOT/usr/share/gofish/icons
install -m644 _gopher+ $RPM_BUILD_ROOT/usr/share/gofish/.gopher+

mkdir -p $RPM_BUILD_ROOT/var/lib/gopherd
install -m644 icons/*.gif $RPM_BUILD_ROOT/var/lib/gopherd/icons
install -m644 _gopher+ $RPM_BUILD_ROOT/var/lib/gopherd/.gopher+

%clean
rm -rf $RPM_BUILD_ROOT

%files
%defattr(-,root,root)
%doc COPYING README INSTALL

/usr/sbin/gopherd
/usr/bin/mkcache
/usr/bin/check-files
/etc/gofish.conf
/etc/rc.d/init.d/gopherd
/usr/man/man1/*
/usr/man/man5/*
/usr/share/gofish/icons/*
/usr/share/gofish/.gopher+

%changelog
* Thu Aug 22 2002 Sean MacLennan <seanm@seanm.ca>
- Updated to 0.8
- Added alpha support for http gateway

* Fri Aug 16 2002 Sean MacLennan <seanm@seanm.ca>
- Updated to 0.7
- Added man pages

* Sat Aug 10 2002 Sean MacLennan <seanm@seanm.ca>
- Updated to 0.4

* Wed Aug  7 2002 Sean MacLennan <seanm@seanm.ca>
- Updated to 0.2

* Fri Aug  2 2002 Sean MacLennan <seanm@seanm.ca>
- Created spec file
