Summary: A Gopher Server
Name: gofish
Version: 0.18
Release: 1
Copyright: GPL
Group: Networking/Daemons
Source: gopher://seanm.ca/9/gofish/gofish-%{version}.tar.gz
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

%package setup
Summary: Initial files for GoFish
Group: Networking/Daemons
Copyright: GPL

%description setup
This package contains the files needed to get GoFish up and running.
Only install this package the first time you install GoFish since it
overwrites files in the gopher root directory.

%prep
%setup
./configure --prefix=/usr --sysconfdir=/etc --localstatedir=/var

%build
make

%install
rm -rf $RPM_BUILD_ROOT

make DESTDIR=$RPM_BUILD_ROOT install

# This is not installed by make install
mkdir -p $RPM_BUILD_ROOT/etc/rc.d/init.d
install init-gofish $RPM_BUILD_ROOT/etc/rc.d/init.d/gopherd

%clean
rm -rf $RPM_BUILD_ROOT

%files
%defattr(-,root,root)
%doc COPYING README INSTALL NEWS AUTHORS ChangeLog

/usr/sbin/gopherd
/usr/bin/mkcache
/usr/bin/check-files
/etc/rc.d/init.d/gopherd
/usr/man/man1/*
/usr/man/man5/*

%files setup
%defattr(-,root,root)
/etc/gofish.conf
/var/lib/gopherd/icons/*
/var/lib/gopherd/.gopher+
/var/lib/gopherd/.cache
/var/lib/gopherd/Configure_GoFish

%changelog
* Sun Sep 22 2002 Sean MacLennan <seanm@seanm.ca>
- Updated to 0.11
- Split into two rpms
- Log fix
- No longer using sendfile

* Sat Aug 24 2002 Sean MacLennan <seanm@seanm.ca>
- Updated to 0.9
- Now using configure

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
