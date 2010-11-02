MAINTAINERCLEANFILES	= Makefile.in

sbin_PROGRAMS		=

if ENABLE_CMAN
sbin_PROGRAMS		+= dlm_controld
endif

noinst_HEADERS		= config.h dlm_controld.h dlm_daemon.h

shared_SOURCES		= action.c cpg.c crc.c deadlock.c main.c \
			  netlink.c plock.c

shared_CPPFLAGS		= -I$(top_srcdir)/dlm/libdlm \
			  -I$(top_srcdir)/dlm/libdlmcontrol \
			  -I$(top_srcdir)/group/include

shared_CFLAGS		= $(logt_CFLAGS) $(cpg_CFLAGS) $(sackpt_CFLAGS)

shared_LIBS		= $(logt_LIBS) $(cpg_LIBS) $(sackpt_LIBS) -lpthread

shared_LDADD		= $(top_builddir)/dlm/libdlm/libdlm.la \
			  $(top_builddir)/dlm/libdlmcontrol/libdlmcontrol.la

if ENABLE_CMAN

dlm_controld_SOURCES	= $(shared_SOURCES) \
			  config.c member_cman.c logging.c

dlm_controld_CPPFLAGS	= $(shared_CPPFLAGS)

dlm_controld_CFLAGS	= $(shared_CFLAGS) \
			  $(ccs_CFLAGS) \
			  $(fenced_CFLAGS) \
			  $(cfg_CFLAGS) \
			  $(quorum_CFLAGS)

dlm_controld_LDFLAGS	= $(shared_LIBS) \
			  $(ccs_LIBS) \
			  $(fenced_LIBS) \
			  $(cfg_LIBS) \
			  $(quorum_LIBS)

dlm_controld_LDADD	= $(shared_LDADD)

endif
