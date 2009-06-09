MAINTAINERCLEANFILES	= Makefile.in

libversion		= 3:0:0

include_HEADERS		= libdlm.h

noinst_HEADERS		= libdlm_internal.h

pkgconfigdir		= $(libdir)/pkgconfig

pkgconfig_DATA		= libdlm.pc libdlm_lt.pc

udevrulesdir		= $(sysconfdir)/udev/rules.d

udevrules_DATA		= $(srcdir)/51-dlm.rules

lib_LTLIBRARIES		= libdlm.la libdlm_lt.la

libdlm_la_SOURCES	= libdlm.c

libdlm_lt_la_SOURCES	= libdlm.c

libdlm_la_CPPFLAGS	= -D_REENTRANT

libdlm_la_LDFLAGS	= -lpthread \
			  -version-info $(libversion)

libdlm_lt_la_LDFLAGS	= -version-info $(libversion)
