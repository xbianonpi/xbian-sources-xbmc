include ../../Makefile.include
DEPS= ../../Makefile.include Makefile tvos.patch

# lib name, version
LIBNAME=libbluray
VERSION=1.1.2
SOURCE=$(LIBNAME)-$(VERSION)
ARCHIVE=$(SOURCE).tar.bz2

# configuration settings
CONFIGURE=./configure --prefix=$(PREFIX) --disable-shared --exec-prefix=$(PREFIX) \
  --disable-examples --disable-doxygen-doc \
  --disable-bdjava-jar

LIBDYLIB=$(PLATFORM)/.libs/libbluray.la

all: .installed-$(PLATFORM)

$(TARBALLS_LOCATION)/$(ARCHIVE):
	cd $(TARBALLS_LOCATION); $(RETRIEVE_TOOL) $(RETRIEVE_TOOL_FLAGS) $(BASE_URL)/$(ARCHIVE)

$(PLATFORM): $(TARBALLS_LOCATION)/$(ARCHIVE) $(DEPS)
	rm -rf $(PLATFORM); mkdir -p $(PLATFORM)
	cd $(PLATFORM); $(ARCHIVE_TOOL) $(ARCHIVE_TOOL_FLAGS) $(TARBALLS_LOCATION)/$(ARCHIVE)
	# libbluray has borked Makefile.am with respect to CFLAGS, remove the offending line
	sed -i -e "s|CFLAGS=|#CFLAGS=|" $(PLATFORM)/Makefile.am
	cd $(PLATFORM); patch -p1 < ../libbluray-01-bump_to_Nevcairiel_vL20170904.patch
	cd $(PLATFORM); patch -p1 < ../libbluray-02-install-extra-MVC-headers.patch
	cd $(PLATFORM); ./bootstrap
	cd $(PLATFORM); $(CONFIGURE)
ifeq ($(TARGET_PLATFORM),appletvos)
	cd $(PLATFORM); patch -p1 -i ../tvos.patch
endif

$(LIBDYLIB): $(PLATFORM)
	$(MAKE) -C $(PLATFORM)

.installed-$(PLATFORM): $(LIBDYLIB)
	$(MAKE) -C $(PLATFORM) install
	touch $@

clean:
	$(MAKE) -C $(PLATFORM) clean
	rm -f .installed-$(PLATFORM)

distclean::
	rm -rf $(PLATFORM) .installed-$(PLATFORM)
