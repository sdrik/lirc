# Retrieve the package version from  the upstream sources
PKG_VERSION    = $(shell grep VERSION sources/VERSION  2>/dev/null \
			    | sed -e 's/.*=//' -e 's/"//g' || echo "unbuilt")

# Retrieve debian version and release from the changelog
VERSION_REL    = $(shell sed -n -e '/^lirc/s/.*(\(.*\)).*/\1/p;q' \
                        < debian/changelog)
DEBIAN_VERSION = $(shell echo $(VERSION_REL) | sed -e 's/-.*//' )
DEBIAN_REL     = $(shell echo $(VERSION_REL) | sed 's/.*-//')

UPSTREAM_SRC   = $(shell cd sources; git ls-files | sed 's|^|sources/|')
UPSTREAM_GZ    = sources/lirc-$(PKG_VERSION).tar.gz
DEBIAN_SRC     = $(shell find debian -type f)
DEBIAN_GZ      = lirc-debian-src-$(DEBIAN_VERSION)-$(DEBIAN_REL).tar.gz
SRCDIR         = lirc-debian-src-$(DEBIAN_VERSION)
DEBIAN_WORKDIR = $(SRCDIR)/lirc-$(DEBIAN_VERSION)/debian
WORKDIR_SRC    = $(shell find $(DEBIAN_WORKDIR) -type f 2>/dev/null || echo "")

UBUNTU_DEVS    = Ubuntu Developers <ubuntu-devel-discuss at lists.ubuntu.com>


all: sid


sid: debian

stretch: debian

jessie: .phony
	cp debian/control debian/control.BAK
	sed -i '/^Standards-Version:/s/:.*/: 3.9.6/' debian/control
	sed -i '/^Maintainer/s/:.*/: $(UBUNTU_DEVS)/' debian/control
	$(MAKE) debian
	mv debian/control.BAK debian/control

trusty:
	cp debian/control debian/control.BAK
	cp debian/NEWS debian/NEWS.BAK
	cp debian/README.Debian debian/README.Debian.BAK
	cp ubuntu.changelog debian/changelog
	sed -i '1 s/experimental/trusty/' debian/README.Debian
	sed -i '1 s/experimental/trusty/' debian/NEWS
	sed -i '/^Standards-Version:/s/:.*/: 3.9.5/' debian/control
	sed -i '/^Maintainer/s/:.*/: $(UBUNTU_DEVS)/' debian/control
	$(MAKE) debian
	mv debian/control.BAK debian/control
	mv debian/NEWS.BAK debian/NEWS
	mv debian/README.Debian.BAK debian/README.debian

xenial:
	cp debian/control debian/control.BAK
	cp debian/NEWS debian/NEWS.BAK
	cp debian/README.Debian debian/README.Debian.BAK
	cp ubuntu.changelog debian/changelog
	sed -i '1 s/experimental/xenial/' debian/README.Debian
	sed -i '1 s/experimental/xenial/' debian/NEWS
	sed -i '/^Standards-Version:/s/:.*/: 3.9.7/' debian/control
	sed -i '1 s/trusty/xenial/' debian/changelog
	$(MAKE) debian
	mv debian/control.BAK debian/control
	mv debian/NEWS.BAK debian/NEWS
	mv debian/README.Debian.BAK debian/README.debian

# If upstream isn't configured version isn't available => build upstream
# and re-invoke make with same targets. Otherwise, run a complete make.

ifeq  ($(PKG_VERSION),)

debian: .phony
	cd sources; ./autogen.sh; ./configure; make dist
	$(MAKE) $(MAKECMDGOALS)
else

debian: $(DEBIAN_GZ)

endif


check-sync: .sync-stamp
	@if test -s  .sync-stamp; then \
	    echo "workspace debian differs from debian:"; \
	    cat .sync-stamp; \
	    exit 1; \
	fi

$(UPSTREAM_GZ): $(UPSTREAM_SRC)
	@echo "WARNING: Modified upstream sources."; sleep 2
	cd sources; make dist

$(DEBIAN_GZ): $(UPSTREAM_GZ) $(DEBIAN_SRC)
	rm -rf $(SRCDIR); mkdir $(SRCDIR)
	cp README.md $(SRCDIR)
	cp sources/lirc-$(PKG_VERSION).tar.gz \
	    $(SRCDIR)/lirc_$(DEBIAN_VERSION).orig.tar.gz
	cd $(SRCDIR) && tar xf lirc_$(DEBIAN_VERSION).orig.tar.gz
	cp -ar debian $(SRCDIR)/lirc-$(PKG_VERSION)
	cd $(SRCDIR)/lirc-$(PKG_VERSION) && debuild -S -us -uc -sa
	rm -r $(SRCDIR)/lirc-$(PKG_VERSION)
##	tar czf $@ $(SRCDIR) && rm -r $(SRCDIR)
	tar czf $@ $(SRCDIR)

sync: .sync-stamp .workspace-stamp
.sync-stamp: .workspace-stamp debian
	rsync -au $(DEBIAN_WORKDIR)/ ./debian/
	rsync -au ./debian/ $(DEBIAN_WORKDIR)/
	diff -r debian $(DEBIAN_WORKDIR) >$@


workspace: .workspace-stamp
.workspace-stamp: debian
	cd $(SRCDIR); dpkg-source -x *.dsc
	touch $@

clean:
	rm -rf $(SRCDIR) sources/debian $(DEBIAN_GZ)
	cd sources; git checkout .; git clean -qxf

.phony:
