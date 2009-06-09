MAINTAINERCLEANFILES	= Makefile.in

noinst_PROGRAMS		= dlmtest asttest lstest pingtest lvb \
			  dlmtest2 flood alternate-lvb joinleave threads

AM_CPPFLAGS		= -D_REENTRANT \
			  -I$(top_srcdir)/dlm/libdlm

dlmtest_LDADD		= $(top_builddir)/dlm/libdlm/libdlm.la
asttest_LDADD		= $(top_builddir)/dlm/libdlm/libdlm.la
lstest_LDADD		= $(top_builddir)/dlm/libdlm/libdlm.la
pingtest_LDADD		= $(top_builddir)/dlm/libdlm/libdlm.la
lvb_LDADD		= $(top_builddir)/dlm/libdlm/libdlm.la
dlmtest2_LDADD		= $(top_builddir)/dlm/libdlm/libdlm.la
flood_LDADD		= $(top_builddir)/dlm/libdlm/libdlm.la
alternate_lvb_LDADD	= $(top_builddir)/dlm/libdlm/libdlm.la
joinleave_LDADD		= $(top_builddir)/dlm/libdlm/libdlm.la
threads_LDADD		= $(top_builddir)/dlm/libdlm/libdlm.la
