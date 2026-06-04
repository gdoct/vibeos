# Build recipe for the mksh port (driven by `pkg create packages/mksh`, which
# runs `make` here — see ../package_info.yml).
#
# mksh has no autoconf/Makefile of its own: it configures and compiles via its
# own Build.sh, which compiles small probe programs and runs them to detect
# target features. We cross-compile with musl-gcc as a *static* binary; such
# binaries also execute on the x86_64 build host, so Build.sh's probes run
# correctly and report the target's (x86_64-linux-musl) characteristics. The
# result runs unmodified on VibeOS (Linux syscall ABI, static — no interpreter).
#
# Output: build/out/bin/mksh  (archived by the manifest as bin/mksh -> /bin/mksh)

VERSION   := R59c
DIST      := mksh-$(VERSION).tgz
# Pinned upstream tarball (canonical MirBSD distribution). It is fetched on
# demand and cached next to this Makefile (the repo's .gitignore excludes *.tgz,
# so the distfile is not committed). The fetch only runs if the file is missing.
# Two mirrors of the *same* bytes (so the pinned SHA-256 stays valid): the https
# endpoint first, then http for hosts whose TLS stack can't negotiate with the
# upstream server.
DIST_URLS := https://www.mirbsd.org/MirOS/dist/mir/mksh/mksh-R59c.tgz \
             http://www.mirbsd.org/MirOS/dist/mir/mksh/mksh-R59c.tgz
DIST_SHA  := 77ae1665a337f1c48c61d6b961db3e52119b38e58884d1c89684af31f87bc506

SRCDIR    := mksh
OUTDIR    := build/out
OUTBIN    := $(OUTDIR)/bin/mksh

# musl cross toolchain. Static + -Os keeps the on-disk binary small.
# NB: a plain `CC ?= musl-gcc` does NOT take effect — make predefines CC=cc with
# origin "default", which `?=` treats as already-set. Override only when CC was
# not set by the environment or command line, so the musl toolchain is the
# default here while still honouring an explicit CC=... from the caller.
ifeq ($(origin CC),default)
CC        := musl-gcc
endif
CFLAGS    ?= -Os
LDFLAGS   ?= -static

all: $(OUTBIN)

# Fetch the distfile only if it is not already cached here. Try each mirror in
# turn; succeed on the first that works.
$(DIST):
	@for url in $(DIST_URLS); do \
	  echo "mksh: fetching $(DIST) from $$url"; \
	  if curl -fsSL -o $@.tmp "$$url"; then mv $@.tmp $@; break; fi; \
	done
	@test -f $@ || { echo "mksh: could not fetch $(DIST) from any mirror"; exit 1; }

# Extract, verifying the pinned checksum first.
$(SRCDIR)/Build.sh: $(DIST)
	@echo "mksh: verifying $(DIST)"
	@echo "$(DIST_SHA)  $(DIST)" | sha256sum -c -
	rm -rf $(SRCDIR)
	tar -xzf $(DIST)
	@test -f $@

# Configure + compile via Build.sh, then strip and stage the binary.
$(OUTBIN): $(SRCDIR)/Build.sh
	@echo "mksh: building (CC=$(CC), static)"
	cd $(SRCDIR) && CC="$(CC)" CFLAGS="$(CFLAGS)" LDFLAGS="$(LDFLAGS)" sh Build.sh -r
	mkdir -p $(OUTDIR)/bin
	cp $(SRCDIR)/mksh $(OUTBIN)
	strip $(OUTBIN)
	@echo "mksh: staged $(OUTBIN)"

clean:
	rm -rf $(SRCDIR) build

# Also remove the cached distfile.
distclean: clean
	rm -f $(DIST)

.PHONY: all clean distclean
