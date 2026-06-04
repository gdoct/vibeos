# Build recipe for the toybox port (driven by `pkg create packages/toybox`,
# which runs `make` here — see ../package_info.yml).
#
# toybox cross-compiles cleanly to a static musl binary. Its defconfig enables a
# few applets that #include Linux UAPI headers (linux/*, asm/*) which the musl
# sysroot doesn't ship; we make those fall through to the host's kernel headers
# with -idirafter (musl's own libc headers still take precedence, so this only
# supplies the missing kernel headers). The static binary runs unmodified on
# VibeOS (Linux syscall ABI, no interpreter).
#
# Output: build/out/bin/toybox  (archived by the manifest as bin/toybox)

VERSION   := 0.8.11
DIST      := toybox-$(VERSION).tar.gz
# Pinned upstream tarball. Fetched on demand and cached next to this Makefile
# (the repo's .gitignore excludes tarballs, so the distfile is not committed).
DIST_URLS := http://landley.net/toybox/downloads/toybox-$(VERSION).tar.gz \
             https://github.com/landley/toybox/archive/refs/tags/$(VERSION).tar.gz
DIST_SHA  := 15aa3f832f4ec1874db761b9950617f99e1e38144c22da39a71311093bfe67dc

SRCDIR    := toybox-$(VERSION)
OUTDIR    := build/out
OUTBIN    := $(OUTDIR)/bin/toybox

# musl cross toolchain. `CC ?= musl-gcc` is a no-op (make predefines CC=cc, origin
# "default"); override only when the caller didn't set CC explicitly.
ifeq ($(origin CC),default)
CC        := musl-gcc
endif
# Supply the missing Linux UAPI headers (incl. the arch-specific asm/ dir) after
# musl's own includes. MULTIARCH is e.g. x86_64-linux-gnu on Debian/Ubuntu.
MULTIARCH := $(shell $(CC) -print-multiarch 2>/dev/null)
KHDRS     := -idirafter /usr/include
ifneq ($(MULTIARCH),)
KHDRS     += -idirafter /usr/include/$(MULTIARCH)
endif

all: $(OUTBIN)

# Fetch the distfile only if it is not already cached here.
$(DIST):
	@for url in $(DIST_URLS); do \
	  echo "toybox: fetching $(DIST) from $$url"; \
	  if curl -fsSL -o $@.tmp "$$url"; then mv $@.tmp $@; break; fi; \
	done
	@test -f $@ || { echo "toybox: could not fetch $(DIST) from any mirror"; exit 1; }

# Extract, verifying the pinned checksum first.
$(SRCDIR)/configure: $(DIST)
	@echo "toybox: verifying $(DIST)"
	@echo "$(DIST_SHA)  $(DIST)" | sha256sum -c -
	rm -rf $(SRCDIR)
	tar -xzf $(DIST)
	@test -d $(SRCDIR)

# Configure (defconfig = all applets) + compile, then strip and stage.
$(OUTBIN): $(SRCDIR)/configure
	@echo "toybox: building (CC=$(CC), static)"
	$(MAKE) -C $(SRCDIR) defconfig
	$(MAKE) -C $(SRCDIR) toybox CC="$(CC)" LDFLAGS="--static" CFLAGS="$(KHDRS)"
	mkdir -p $(OUTDIR)/bin
	cp $(SRCDIR)/toybox $(OUTBIN)
	chmod u+w $(OUTBIN)        # toybox builds read-only; make writable so strip can edit in place
	strip $(OUTBIN)
	@echo "toybox: staged $(OUTBIN)"

clean:
	rm -rf $(SRCDIR) build

distclean: clean
	rm -f $(DIST)

.PHONY: all clean distclean
