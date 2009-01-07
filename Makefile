include make/defines.mk


REALSUBDIRS = dlm group doc \
	      bindings contrib

SUBDIRS = $(filter-out \
	  $(if ${without_dlm},dlm) \
	  $(if ${without_group},group) \
	  $(if ${without_bindings},bindings) \
	  , $(REALSUBDIRS))

all: ${SUBDIRS}

${SUBDIRS}:
	[ -n "${without_$@}" ] || ${MAKE} -C $@ all

# Dependencies

dlm: 
group: dlm
bindings: 
contrib: 

oldconfig:
	@if [ -f $(OBJDIR)/.configure.sh ]; then \
		sh $(OBJDIR)/.configure.sh; \
	else \
		echo "Unable to find old configuration data"; \
	fi

install:
	set -e && for i in ${SUBDIRS}; do ${MAKE} -C $$i $@; done

uninstall:
	set -e && for i in ${SUBDIRS}; do ${MAKE} -C $$i $@; done

clean:
	set -e && for i in ${REALSUBDIRS}; do \
		contrib_code=1 \
		legacy_code=1 \
		${MAKE} -C $$i $@;\
	done

distclean: clean
	rm -f make/defines.mk
	rm -f .configure.sh
	rm -f *tar.gz
	rm -rf build

.PHONY: ${REALSUBDIRS}
