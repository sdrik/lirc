# Retrieve the package version from  the upstream sources
PKG_VERSION    = $(shell grep VERSION sources/VERSION  2>/dev/null \
			    | sed -e 's/.*=//' -e 's/"//g' || echo "unbuilt")

FEDORA_VERSION = 0.9.4
FEDORA_REL     = 1

UPSTREAM_SRC   = $(shell cd sources; git ls-files | sed 's|^|sources/|')
UPSTREAM_GZ    = sources/lirc-$(PKG_VERSION).tar.gz
FEDORA_SRC     = $(shell find fedora -type f)


all: rpms

rpms: tarball
	cd fedora; rpmbuild -D "_sourcedir $$PWD" -D "_srpmdir $$PWD"  \
	    -ba *.spec


ifeq  ($(PKG_VERSION),)

tarball: .phony
	cd sources; ./autogen.sh; ./configure; make dist
	$(MAKE) $(MAKECMDGOALS)
else

tarball: $(UPSTREAM_GZ)

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
