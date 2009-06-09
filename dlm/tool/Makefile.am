MAINTAINERCLEANFILES	= Makefile.in

sbin_PROGRAMS		= dlm_tool

dlm_tool_SOURCES	= main.c

dlm_tool_CPPFLAGS	= -I$(top_srcdir)/dlm/libdlm \
			  -I$(top_srcdir)/dlm/libdlmcontrol \
			  -I$(top_srcdir)/group/dlm_controld

dlm_tool_LDADD		= $(top_builddir)/dlm/libdlm/libdlm.la \
			  $(top_builddir)/dlm/libdlmcontrol/libdlmcontrol.la
