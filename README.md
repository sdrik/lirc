# LIRC Debian packaging README

### General

The debian packaging lives in two branches  'debian' and 'ubuntu' in the
lirc project.  The goal is to provide modern lirc packages for debian and
ubuntu. At the time of writing it provides 0.9.4 packages.

The 'debian' branch is used for the official packaging available in
Debian (stretch) and Ubuntu (zesty). The 'ubuntu' branch builds on
debian (jessie) and ubuntu (trusty, xenial and yakkety).

The packaging provides a debian source package. Such a source package
can be built using git clone (below). Lirc releases also contains a
prebuilt source package [1], built from the  'ubuntu' branch.

To use the source package it must be used to build binary .deb packages.
Describing this procedure is the main purpose of this document.


### Rebuilding the source package from git

To build the debian source package clone and run make using something like:

    $ git clone --recursive -b ubuntu git://git.code.sf.net/p/lirc/git lirc-pkg
    $ cd lirc-pkg
    $ make

This creates a tarball lirc-debian-src-0.9.4-1.3.tar.gz. See below for
creating binary .deb packages from the debian sources.

Creating the source package is also tested on Fedora. This requires
leamas's dpkg-addons repo and some packages:

    # dnf copr enable leamas/dpkg-addons
    # dnf install dpkg-addons dpkg dpkg-dev devscripts

The imported upstream lirc sources lives in the "sources" submodule. It
is typically committed as last release i. e., the --recursive clone
command (above) checks out this.


### Building deb packages on Debian

To build the debian package download the tarball from sourceforge [1] or
rebuild it from git (above) and do something similar to

    $ tar xf lirc-debian-src-0.9.4-1.3.tar.gz
    $ cd lirc-debian-src-0.9.4
    $ sudo apt-get install pbuilder
    $ sudo pbuilder create --distribution jessie  --override-config
    $ sudo rm -f /var/cache/pbuilder/result/*
    $ sudo pbuilder build *.dsc

The build results in a number of packages in */var/cache/pbuilder/result/*. A
complete install example:

    $ sudo dpkg -i $(ls /var/cache/pbuilder/result/*.deb | grep -v dbgsym)
    $ sudo apt-get install -f --fix-missing

You might want to exclude some optional packages to trim the installed size:

   - lirc-x: Not large, but pulls in the X libraries.
   - lirc-doc: Somewhat large package around 1 MB.
   - lirc\*-dev: The development packages are useless if not developing
     programs using lirc.


### Building deb packages on Ubuntu

The debian sources are known to build on Trusty (14.04) and Xenial (16.04).

First, enable universe for pbuilder by adding this line to ~/.pbuilderrc
(which might not exist):

    echo 'COMPONENTS="main restricted universe"' >> ~/.pbuilderrc

Check the results - if there is two *COMPONENTS=* in .pbuilderrc it must be
cleaned up. Then proceed with the same steps as for Debian, but use a
ubuntu distro instead of a debian one i. e.,

    $ sudo pbuilder create --distribution trusty  --override-config
    ## Or 'xenial', 'yakkety'.


### Building a patched version

The pbuilder-based workflow is designed to rebuild the sources as-is without
further modifications. To make it possible to patch the sources instead use
debuild(3) and friends. First, unpack the sources;

    $ sudo apt-get install devscripts
    $ tar xf lirc-debian-src-0.9.4-1.tar.gz
    $ cd debian-src
    $ dpkg-source -x *dsc
    $ cd lirc-0.9.4

Then, install the build dependencies:

    $ mk-build-deps
    $ sudo dpkg -i lirc-build-deps*.deb
    $ sudo apt-get install -f --fix-missing
    $ rm lirc-build-deps*.deb

At this point all is set to modify the sources. If anything outside the
debian directory is changed it should be committed:

    $ dpkg-source --commit

Then rebuild using e. g.,

    $ debuild -us -uc


## References

[1] https://sourceforge.net/projects/lirc/files/LIRC/
