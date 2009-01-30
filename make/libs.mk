# handle objects
ifndef OBJS
	OBJS = $(TARGET).o
endif

# we always build the static version
ifndef STATICLIB
	STATICLIB = $(TARGET).a
endif

# handle the shared version
ifndef MAKESTATICLIB
	ifndef LIBDIRT
		LIBDIRT=$(TARGET).a \
			$(TARGET).so.$(SOMAJOR).$(SOMINOR)
	endif
	ifndef LIBSYMT
		LIBSYMT=$(TARGET).so \
			$(TARGET).so.$(SOMAJOR)
	endif
	ifndef INCDIRT
		INCDIRT=$(TARGET).h
	endif
	ifndef SHAREDLIB
		SHAREDLIB=$(TARGET).so.${SOMAJOR}.${SOMINOR}
	endif
	ifndef PKGCONF
		PKGCONF=$(TARGET).pc
	endif

all: $(STATICLIB) $(SHAREDLIB) $(PKGCONF)

$(SHAREDLIB): $(OBJS)
	$(CC) -shared -o $@ -Wl,-soname=$(TARGET).so.$(SOMAJOR) $^ $(LDFLAGS)
	ln -sf $(TARGET).so.$(SOMAJOR).$(SOMINOR) $(TARGET).so
	ln -sf $(TARGET).so.$(SOMAJOR).$(SOMINOR) $(TARGET).so.$(SOMAJOR)

$(PKGCONF): $(S)/$(PKGCONF).in
	cat $(S)/$(PKGCONF).in | \
	sed \
		-e 's#@PREFIX@#${prefix}#g' \
		-e 's#@LIBDIR@#${libdir}#g' \
		-e 's#@INCDIR@#${incdir}#g' \
		-e 's#@VERSION@#${RELEASE_VERSION}#g' \
	> $@

else

all: $(STATICLIB)

endif

$(STATICLIB): $(OBJS)
	${AR} cru $@ $^
	${RANLIB} $@

clean: generalclean

-include $(OBJS:.o=.d)
