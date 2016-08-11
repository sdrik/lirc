## Process this file with automake to produce Makefile.in
ACLOCAL_AMFLAGS           = -I m4

AM_CPPFLAGS               = -I$(top_srcdir) -I$(top_srcdir)/lib -Wall
AM_CPPFLAGS               += -std=c++11 -Wp,-D_FORTIFY_SOURCE=2 @osflags@
AM_LDFLAGS                = -lpthread $(SYSTEMD_LIBS)

CLEANFILES                = *~

varrundir                 = $(localstatedir)/run

bin_PROGRAMS              = lircrcd

lircrcd_SOURCES           = lircrcd.cpp
lircrcd_LDADD             = ../lib/liblirc_client.la

sbin_PROGRAMS             = lircd lircmd lircd-backend-tcp lircd-backend-std

if HAVE_UINPUT
sbin_PROGRAMS             += lircd-uinput
endif

lircd_SOURCES             = lircd.cpp lircd_\
                            fd_list.h fd_list.cpp \
                            lircd_commands.cpp \
                            lircd_options.h lircd_options.cpp

lircd_LDADD               = ../lib/liblirc.la

lircd_uinput_SOURCES      = lircd-uinput.cpp
lircd_uinput_LDADD        = ../lib/liblirc.la

lircd_backend_tcp_SOURCES = lircd-backend-tcp.cpp
lircd_backend_tcp_LDADD   = ../lib/liblirc.la

lircd_backend_std_SOURCES = lircd-backend-std.cpp backend-commands.cpp
lircd_backend_std_LDADD   = ../lib/liblirc.la

lircmd_SOURCES            = lircmd.cpp
lircmd_LDADD              = ../lib/liblirc.la

cpplint:
	../git-tools/cpplint.py $(lircd_SOURCES) \
	    $(lircrcd_SOURCES) \
            $(lircd_uinput_SOURCES) \
	    $(lircmd_SOURCES) \
	    $(lircd_backend_tcp_SOURCES)

install-exec-local:
	test -d  $(DESTDIR)$(varrundir)/$(PACKAGE) \
		 || mkdir -p $(DESTDIR)$(varrundir)/$(PACKAGE)
uninstall-local:
	-$(RM) $(DESTDIR)$(varrundir)/$(PACKAGE)/lircd

checkfiles:
	../git-tools/checkfiles $(SOURCES)
