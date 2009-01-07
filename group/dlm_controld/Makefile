include ../../make/defines.mk

ifdef enable_pacemaker
TARGET = dlm_controld.pcmk
SBINDIRT = dlm_controld.pcmk
else
TARGET = dlm_controld
SBINDIRT = dlm_controld
endif

all: ${TARGET}

include $(OBJDIR)/make/cobj.mk
include $(OBJDIR)/make/clean.mk
include $(OBJDIR)/make/install.mk
include $(OBJDIR)/make/uninstall.mk

OBJS=	action.o \
	cpg.o \
	crc.o \
	deadlock.o \
	main.o \
	netlink.o \
	plock.o \
	group.o

ifdef enable_pacemaker
OBJS +=	pacemaker.o
else
OBJS +=	config.o \
	member_cman.o \
	logging.o
endif

CFLAGS += -I${ccsincdir} -I${cmanincdir} -I${logtincdir} 
CFLAGS += -I${dlmincdir} -I${dlmcontrolincdir}
CFLAGS += -I${corosyncincdir} -I${openaisincdir}
CFLAGS += -I${groupincdir} -I${fencedincdir}
CFLAGS += -I${KERNEL_SRC}/include/
CFLAGS += -I$(S)/../include/
ifdef enable_pacemaker
CFLAGS += -I${incdir}/heartbeat -I${incdir}/pacemaker
CFLAGS += `pkg-config glib-2.0 --cflags`
CFLAGS += `xml2-config --cflags`
endif
CFLAGS += -I${incdir}

LDFLAGS += -L${dlmlibdir} -ldlm 
LDFLAGS += -L${logtlibdir} -llogthread
LDFLAGS += -L${openaislibdir} -lSaCkpt
LDFLAGS += -L${corosynclibdir} -lcpg 
LDFLAGS += -L${grouplibdir} -lgroup
LDFLAGS += -L${libdir}

LDDEPS += ${grouplibdir}/libgroup.a

PCMK_LDFLAGS += -lcib -lcrmcommon -lcrmcluster -ltotem_pg
PCMK_LDFLAGS += `pkg-config glib-2.0 --libs`
PCMK_LDFLAGS += `xml2-config --libs`

CMAN_LDFLAGS += -L${ccslibdir} -L${cmanlibdir} -lccs -lcman
CMAN_LDFLAGS += -L${fencedlibdir} -lfenced
CMAN_LDDEPS += ${fencedlibdir}/libfenced.a

dlm_controld: ${OBJS} ${LDDEPS} ${CMAN_LDDEPS}
	$(CC) -o $@ $^ $(LDFLAGS) $(CMAN_LDFLAGS)

dlm_controld.pcmk: ${OBJS} ${LDDEPS} ${PCMK_LDDEPS}
	$(CC) -o $@ $^ $(LDFLAGS) $(PCMK_LDFLAGS)

clean: generalclean

-include $(OBJS:.o=.d)
