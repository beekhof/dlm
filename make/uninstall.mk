uninstall:
ifdef LIBDIRT
	${UNINSTALL} ${LIBDIRT} ${libdir}
endif
ifdef LIBSYMT
	${UNINSTALL} ${LIBSYMT} ${libdir}
endif
ifdef INCDIRT
	${UNINSTALL} ${INCDIRT} ${incdir}
endif
ifdef SBINDIRT
	${UNINSTALL} ${SBINDIRT} ${sbindir}
endif
ifdef SBINSYMT
	${UNINSTALL} ${SBINSYMT} ${sbindir}
endif
ifdef UDEVT
	${UNINSTALL} ${UDEVT} ${DESTDIR}/etc/udev/rules.d
endif
ifdef DOCS
	${UNINSTALL} ${DOCS} ${docdir}
endif
ifdef PKGCONF
	${UNINSTALL} ${PKGCONF} ${pkgconfigdir}
endif
