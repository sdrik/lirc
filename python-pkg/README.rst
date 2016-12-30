Python API for lirc
===================

This package is created from the upstream lirc project's main build
flow. If you have installed LIRC  either from a package or directly
from sources you don't need this. The primary usecase is when installing
the LIRC api in a virtual environment.

Note that the receiving parts are based on a C module linked to LIRC. Thus
it's not possible to receive data without the complete LIRC package
installed.

Dependencies
------------
    - A LIRC installation complete with header files (typically
      the -dev or -devel packages, or built from source).
    - Tests requires ncat and expect.


Installation
------------

    - Build from source or install a LIRC package.
    - Locate the source distribution, typically in a location like
      /usr/share/lirc/lirc-0.9.5.tar.gz
    - Use cmd like  *pip install /usr/share/lirc/lirc-0.9.5.tar.gz*.
    - As a shortcut, the package is available in python-pkg/dist after
      a plain *make*.


Documentation
-------------

See the main LIRC API docs, look for "Python bindings" under Modules. It
is also possible to run Doxygen in the package doc/ directory to create
documents for just the python package.


See also:
---------

https://github.com/pylover/aiolirc
