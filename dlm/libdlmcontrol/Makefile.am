MAINTAINERCLEANFILES	= Makefile.in

libversion		= 3:0:0

include_HEADERS		= libdlmcontrol.h

pkgconfigdir		= $(libdir)/pkgconfig

pkgconfig_DATA		= libdlmcontrol.pc

lib_LTLIBRARIES		= libdlmcontrol.la

libdlmcontrol_la_SOURCES  = main.c

libdlmcontrol_la_CPPFLAGS = -I${top_srcdir}/group/dlm_controld

libdlmcontrol_la_LDFLAGS  = -version-info $(libversion)
