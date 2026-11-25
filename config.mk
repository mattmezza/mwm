# mwm version
VERSION = 0.4.0

PREFIX    = /usr/local
MANPREFIX = $(PREFIX)/share/man

# Comment these lines out to build without Xinerama.
XINERAMALIBS  = -lXinerama
XINERAMAFLAGS = -DXINERAMA

CPPFLAGS = -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -DVERSION=\"$(VERSION)\" $(XINERAMAFLAGS) -I$(CUI_SOURCE)/include
CFLAGS   = -std=c11 -pedantic -Wall -Wextra -Wno-deprecated-declarations -Os
LIBS     = -lX11 -lXext -lXrender $(XINERAMALIBS) -lfontconfig -lfreetype -lharfbuzz -lm
X11_LIBS = -lX11
TRAY_CFLAGS = $(shell pkg-config --cflags gio-2.0)
TRAY_LIBS = $(shell pkg-config --libs gio-2.0)
CC = cc
