# Build recipe for the Doom package (driven by `pkg create packages/doom`, which
# runs `make` here — see ../package_info.yml).
#
# The upstream DoomGeneric source is fetched on demand from a pinned GitHub
# tarball, verified by SHA-256, unpacked into build/src/doomgeneric, and patched
# with the package-owned VibeOS backend before compilation.

VERSION := 1.0.0
OUTDIR  := build/out
OUTBIN  := $(OUTDIR)/bin/doom
OUTDOC  := $(OUTDIR)/share/doc/doom
OBJDIR  := build/obj

DISTDIR  := build/dist
UPROOT   := build/src/doomgeneric
SRCROOT  := $(UPROOT)/doomgeneric
STAMP    := $(UPROOT)/.prepared

DIST_COMMIT := dcb7a8dbc7a16ce3dda29382ac9aae9d77d21284
DIST       := $(DISTDIR)/doomgeneric-$(DIST_COMMIT).tar.gz
DIST_URL   := https://github.com/ozkl/doomgeneric/archive/$(DIST_COMMIT).tar.gz
DIST_SHA   := 1bd3f7f26220494159a38d71f2847ec81b58d6bbd7c7c8d81b08993018001148

ifeq ($(origin CC),default)
CC := musl-gcc
endif

CFLAGS  ?= -O2
LDFLAGS ?= -static -no-pie
CPPFLAGS += -DNORMALUNIX -DLINUX -DSNDSERV -D_DEFAULT_SOURCE \
			-I$(SRCROOT) -Igui/include
LIBS    += -lm

SRC_DOOM = \
	dummy.c am_map.c doomdef.c doomstat.c dstrings.c d_event.c d_items.c \
	d_iwad.c d_loop.c d_main.c d_mode.c d_net.c f_finale.c f_wipe.c \
	g_game.c hu_lib.c hu_stuff.c info.c i_cdmus.c i_endoom.c i_joystick.c \
	i_scale.c i_sound.c i_system.c i_timer.c memio.c m_argv.c m_bbox.c \
	m_cheat.c m_config.c m_controls.c m_fixed.c m_menu.c m_misc.c \
	m_random.c p_ceilng.c p_doors.c p_enemy.c p_floor.c p_inter.c \
	p_lights.c p_map.c p_maputl.c p_mobj.c p_plats.c p_pspr.c p_saveg.c \
	p_setup.c p_sight.c p_spec.c p_switch.c p_telept.c p_tick.c p_user.c \
	r_bsp.c r_data.c r_draw.c r_main.c r_plane.c r_segs.c r_sky.c \
	r_things.c sha1.c sounds.c statdump.c st_lib.c st_stuff.c s_sound.c \
	tables.c v_video.c wi_stuff.c w_checksum.c w_file.c w_main.c w_wad.c \
	z_zone.c w_file_stdc.c i_input.c i_video.c doomgeneric.c mus2mid.c \
	doomgeneric_vibeos.c

SRC_GUI = \
	gui_client.c libgfx.c font_chicago.c font_gomono.c

OBJS = $(addprefix $(OBJDIR)/doomgeneric/,$(SRC_DOOM:.c=.o)) \
	   $(addprefix $(OBJDIR)/gui/,$(SRC_GUI:.c=.o))

all: $(OUTBIN) $(OUTDOC)/README $(OUTDOC)/LICENSE

$(DIST):
	mkdir -p $(DISTDIR)
	@echo "doom: fetching DoomGeneric $(DIST_COMMIT)"
	curl -fsSL -o $@.tmp $(DIST_URL)
	mv $@.tmp $@

$(STAMP): $(DIST) doomgeneric_vibeos.c
	@echo "doom: verifying $(notdir $(DIST))"
	@echo "$(DIST_SHA)  $(DIST)" | sha256sum -c -
	rm -rf $(UPROOT)
	mkdir -p $(UPROOT)
	tar -xzf $(DIST) -C $(UPROOT) --strip-components=1
	cp doomgeneric_vibeos.c $(SRCROOT)/doomgeneric_vibeos.c
	touch $@

$(OUTBIN): $(OBJS)
	@echo "doom: linking $(OUTBIN)"
	mkdir -p $(OUTDIR)/bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)
	strip $@

$(OUTDOC)/README: ../README.md
	mkdir -p $(OUTDOC)
	cp $< $@

$(OUTDOC)/LICENSE: $(STAMP) $(UPROOT)/LICENSE
	mkdir -p $(OUTDOC)
	cp $(UPROOT)/LICENSE $@


$(OBJDIR)/doomgeneric/%.o: $(STAMP) | $(OBJDIR)/doomgeneric
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $(SRCROOT)/$*.c -o $@

$(OBJDIR)/gui/%.o: gui/src/%.c | $(OBJDIR)/gui
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OBJDIR)/doomgeneric $(OBJDIR)/gui:
	mkdir -p $@

clean:
	rm -rf build

.PHONY: all clean