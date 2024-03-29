install:
ifdef LIBDIRT
	install -d ${libdir}
	install -m644 ${LIBDIRT} ${libdir}
endif
ifdef LIBSYMT
	cp -a ${LIBSYMT} ${libdir}
endif
ifdef INCDIRT
	install -d ${incdir}
	set -e; \
	for i in ${INCDIRT}; do \
		install -m644 $(S)/$$i ${incdir}; \
	done
endif
ifdef SBINDIRT
	install -d ${sbindir}
	install -m755 ${SBINDIRT} ${sbindir}
endif
ifdef SBINSYMT
	cp -a ${SBINSYMT} ${sbindir}
endif
ifdef UDEVT
	install -d ${DESTDIR}/etc/udev/rules.d
	set -e; \
	for i in ${UDEVT}; do \
		install -m644 $(S)/$$i ${DESTDIR}/etc/udev/rules.d; \
	done
endif
ifdef DOCS
	install -d ${docdir}
	set -e; \
	for i in ${DOCS}; do \
		install -m644 $(S)/$$i ${docdir}; \
	done
endif
ifdef PKGCONF
	install -d ${pkgconfigdir}
	install -m644 ${PKGCONF} ${pkgconfigdir}
endif
