# If the gopher directory exists don't touch it!
if [ ! -d var/gopher ] ; then
  mv var/gopher.new var/gopher
  chown -R nobody.nobody var/gopher
fi
rm -rf var/gopher.new

# Don't touch existing config file
[ -f etc/gofish.conf ] || mv etc/gofish.conf.new etc/gofish.conf
