MAINTAINERCLEANFILES	= Makefile.in

sbin_PROGRAMS		= dlm_controld

noinst_HEADERS		= config.h dlm_controld.h dlm_daemon.h

dlm_controld_SOURCES	= action.c cpg.c crc.c deadlock.c main.c \
			  netlink.c plock.c config.c member_cman.c \
			  logging.c rbtree.c

dlm_controld_CPPFLAGS	= -I$(top_srcdir)/dlm/libdlm \
			  -I$(top_srcdir)/dlm/libdlmcontrol \
			  -I$(top_srcdir)/group/include

dlm_controld_CFLAGS	= $(logt_CFLAGS) \
			  $(cpg_CFLAGS) \
			  $(sackpt_CFLAGS) \
			  $(confdb_CFLAGS)
			  $(ccs_CFLAGS) \
			  $(fenced_CFLAGS) \
			  $(cfg_CFLAGS) \
			  $(quorum_CFLAGS)

dlm_controld_LDFLAGS	= $(logt_LIBS) \
			  $(cpg_LIBS) \
			  $(sackpt_LIBS) \
			  $(confdb_LIBS) \
			  $(ccs_LIBS) \
			  $(fenced_LIBS) \
			  $(cfg_LIBS) \
			  $(quorum_LIBS) \
			  -lpthread

dlm_controld_LDADD	= $(top_builddir)/dlm/libdlm/libdlm.la \
			  $(top_builddir)/dlm/libdlmcontrol/libdlmcontrol.la
