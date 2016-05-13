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


all: debian


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
