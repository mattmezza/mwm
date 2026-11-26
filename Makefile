# mwm - matteo's window manager
.POSIX:

include config.mk

CUI_REVISION = 4752885b8a8f8eec40d1c91964f1f57e4d8d37e1
CUI_SOURCE = .deps/cui-$(CUI_REVISION)
CUI_BUILD = .deps/cui-build-$(CUI_REVISION)
CUI_READY = $(CUI_BUILD)/.mwm-built
CUI_LIBS = $(CUI_BUILD)/libcui_software.a $(CUI_BUILD)/libcui_text.a \
           $(CUI_BUILD)/libcui_bidi.a $(CUI_BUILD)/libcui.a

SRC = mwm.c mwm_config.c shell.c controls.c activity.c util.c
OBJ = $(SRC:.c=.o)
TRAY_OBJ = tray.o tray_ui.o tray_bus.o tray_wifi.o tray_secrets.o tray_bluetooth.o tray_nod.o tray_displays.o

all: mwm mwmc mwm-tray

.c.o:
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $<

mwm.o shell.o mwm_config.o controls.o activity.o: $(CUI_READY) shell.h controls.h activity.h mwm_config.h
shell.o: font_assets.h
mwm.o mwmc.o util.o: util.h config.mk
mwm.o mwmc.o: ipc.h

$(CUI_SOURCE)/.mwm-source:
	mkdir -p $(CUI_SOURCE)
	curl -fsSL --retry 3 https://codeload.github.com/mattmezza/cui/tar.gz/$(CUI_REVISION) -o $(CUI_SOURCE)/cui.tar.gz
	tar -xzf $(CUI_SOURCE)/cui.tar.gz --strip-components=1 -C $(CUI_SOURCE)
	touch $@

$(CUI_READY): $(CUI_SOURCE)/.mwm-source
	cmake -S $(CUI_SOURCE) -B $(CUI_BUILD) -DCMAKE_BUILD_TYPE=Release \
	    -DCUI_BUILD_NATIVE=ON -DCUI_BUILD_CPP_EXAMPLE=OFF
	cmake --build $(CUI_BUILD) --target cui_software --parallel 8
	test -f $(CUI_BUILD)/libcui_text.a
	touch $@

mwm: $(OBJ) $(CUI_READY)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(CUI_LIBS) $(LIBS)

mwmc: mwmc.o util.o
	$(CC) $(LDFLAGS) -o $@ mwmc.o util.o $(X11_LIBS)

$(TRAY_OBJ): %.o: %.c $(CUI_READY) shell.h mwm_config.h tray.h config.mk
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TRAY_CFLAGS) -c $<

tray.o tray_ui.o: tray_window.h
tray_wifi.o tray_secrets.o: tray_wifi.h

mwm-tray: $(TRAY_OBJ) shell.o mwm_config.o controls.o activity.o $(CUI_READY)
	$(CC) $(LDFLAGS) -o $@ $(TRAY_OBJ) shell.o mwm_config.o controls.o activity.o $(CUI_LIBS) $(LIBS) $(TRAY_LIBS)

mwmc.o: mwmc.c completions.h

# embed the shell completion scripts into mwmc (so the binary alone can emit
# them via `mwmc completions bash|zsh`). Each file is turned into a C string
# literal: escape backslashes, then double-quotes, then wrap each line.
completions.h: completions/mwmc.bash completions/mwmc.zsh
	{ \
	printf 'static const char completion_bash[] =\n'; \
	sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' completions/mwmc.bash; \
	printf ';\nstatic const char completion_zsh[] =\n'; \
	sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' completions/mwmc.zsh; \
	printf ';\n'; \
	} > $@

font_assets.h: fonts/Geist-Regular.ttf fonts/Geist-Medium.ttf fonts/GeistMono-Regular.ttf fonts/OFL.txt
	@set -e; for asset in $^; do \
	    name=$$(basename "$$asset" | tr '.-' '__'); \
	    printf 'static const unsigned char %s[] = {\n' "$$name"; \
	    od -An -v -tu1 "$$asset" | awk '{ for (i = 1; i <= NF; ++i) printf "%s,", $$i; print "" }'; \
	    printf '};\n'; \
	done > $@

clean:
	rm -f mwm mwmc mwm-tray $(OBJ) $(TRAY_OBJ) mwmc.o completions.h font_assets.h mwm-$(VERSION).tar.gz

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	dir="$(DESTDIR)$(PREFIX)/bin"; stage=$$(mktemp -d "$$dir/.mwm-install.XXXXXXXX") && \
	trap 'rm -rf "$$stage"' EXIT && \
	install -m 755 mwm mwmc mwm-tray "$$stage" && \
	mv -f "$$stage/mwm-tray" "$$dir/mwm-tray" && \
	mv -f "$$stage/mwmc" "$$dir/mwmc" && mv -f "$$stage/mwm" "$$dir/mwm"
	mkdir -p $(DESTDIR)$(MANPREFIX)/man1
	sed "s/VERSION/$(VERSION)/g" mwm.1 > $(DESTDIR)$(MANPREFIX)/man1/mwm.1 2>/dev/null || true
	sed "s/VERSION/$(VERSION)/g" mwmc.1 > $(DESTDIR)$(MANPREFIX)/man1/mwmc.1 2>/dev/null || true
	chmod 644 $(DESTDIR)$(MANPREFIX)/man1/mwm.1 2>/dev/null || true
	chmod 644 $(DESTDIR)$(MANPREFIX)/man1/mwmc.1 2>/dev/null || true
	mkdir -p $(DESTDIR)$(PREFIX)/share/bash-completion/completions
	cp -f completions/mwmc.bash $(DESTDIR)$(PREFIX)/share/bash-completion/completions/mwmc
	mkdir -p $(DESTDIR)$(PREFIX)/share/zsh/site-functions
	cp -f completions/mwmc.zsh $(DESTDIR)$(PREFIX)/share/zsh/site-functions/_mwmc

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/mwm \
	      $(DESTDIR)$(PREFIX)/bin/mwmc \
	      $(DESTDIR)$(PREFIX)/bin/mwm-tray \
	      $(DESTDIR)$(MANPREFIX)/man1/mwm.1 \
	      $(DESTDIR)$(MANPREFIX)/man1/mwmc.1 \
	      $(DESTDIR)$(PREFIX)/share/bash-completion/completions/mwmc \
	      $(DESTDIR)$(PREFIX)/share/zsh/site-functions/_mwmc

release:
	@set -eu; tag=$${name:-}; \
	case "$$tag" in \
	    *[!v0-9.]*) echo 'Expected a release tag such as v0.1.0.' >&2; exit 1 ;; \
	esac; \
	printf '%s\n' "$$tag" | LC_ALL=C grep -Exq 'v(0|[1-9][0-9]*)(\.(0|[1-9][0-9]*)){2}' \
	    || { echo 'Expected a release tag such as v0.1.0.' >&2; exit 1; }; \
	test -z "$$(git status --porcelain)" || { echo 'Commit the release changes first.' >&2; exit 1; }; \
	if existing=$$(git rev-parse --verify --quiet "refs/tags/$$tag^{commit}"); then \
	    test "$$existing" = "$$(git rev-parse HEAD)" || { echo "Tag $$tag already names a different commit." >&2; exit 1; }; \
	else \
	    git tag -a "$$tag" -m "mwm $$tag"; \
	fi; \
	git push --no-follow-tags origin "refs/tags/$$tag"

# --- vagrant test VM helpers (run on host) ---
vmup:
	vagrant up --provider=libvirt

vmconnect: vmup
	virt-viewer --connect qemu:///system --attach mwm_default

vmdown:
	vagrant destroy

.PHONY: all clean install uninstall release vmup vmconnect
