APP_NAME := rill
KRYON_DIR ?= ../kryon
PLAN9PORT_DIR ?= ../plan9port
KRYON_BACKEND ?= libdraw

CC ?= cc
CFLAGS ?= -Wall -Wextra -O2
CPPFLAGS := -Iinclude -I$(KRYON_DIR)/include
LDFLAGS := -rdynamic
LDLIBS =
GTK_PKG_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 gio-unix-2.0 2>/dev/null)
GTK_PKG_LIBS := $(shell pkg-config --libs gtk+-3.0 gio-unix-2.0 2>/dev/null)
X11_PKGS := x11 xcomposite xdamage xfixes xrender xtst
X11_PKG_CFLAGS := $(shell pkg-config --cflags $(X11_PKGS) 2>/dev/null)
X11_PKG_LIBS := $(shell pkg-config --libs $(X11_PKGS) 2>/dev/null || printf '%s' '-lX11 -lXcomposite -lXdamage -lXfixes -lXrender -lXtst')

UNAME_S := $(shell uname -s 2>/dev/null)
UNAME_M := $(shell uname -m 2>/dev/null)
ifeq ($(UNAME_M),amd64)
  ARCH := x86_64
else
  ARCH := $(UNAME_M)
endif
ifeq ($(UNAME_S),Linux)
  PLATFORM := linux
  PLATFORM_SRC := src/platform_linux.c src/rill_wayland.c src/session.c
  ifneq ($(shell pkg-config --exists wayland-client && command -v wayland-scanner),)
    WAYLAND_SRC := build/protocols/toplevel-protocol.c
    CPPFLAGS += -DRILL_HAS_WAYLAND -Ibuild/protocols $(shell pkg-config --cflags wayland-client)
    LDLIBS += $(shell pkg-config --libs wayland-client)
  endif
  PLATFORM_LDLIBS := $(X11_PKG_LIBS) $(shell pkg-config --libs sm ice) -ldl -lrt
else
  PLATFORM := unknown
  PLATFORM_SRC := src/platform_stub.c
  PLATFORM_LDLIBS :=
endif

KRYON_BUILD_ROOT := $(abspath build/kryon-$(KRYON_BACKEND))
KRYON_BUILD_DIR := $(KRYON_BUILD_ROOT)/$(PLATFORM)-$(ARCH)
KRYON_LIB := $(KRYON_BUILD_DIR)/libkryon.a
BOX2D_A := $(KRYON_BUILD_DIR)/vendor/box2d/src/libbox2d.a
LIBOQS_A := $(KRYON_BUILD_DIR)/vendor/liboqs/lib/liboqs.a
CURL_A := $(KRYON_BUILD_DIR)/vendor/curl/lib/libcurl.a
CMARK_A := $(KRYON_BUILD_DIR)/vendor/cmark-gfm/src/libcmark-gfm.a
CMARK_EXT_A := $(KRYON_BUILD_DIR)/vendor/cmark-gfm/extensions/libcmark-gfm-extensions.a
CURL_CODEC_LDLIBS := $(strip \
  $(shell pkg-config --libs libbrotlidec 2>/dev/null) \
  $(shell pkg-config --libs libbrotlicommon 2>/dev/null) \
  $(shell pkg-config --libs libzstd 2>/dev/null))
LDLIBS += -Wl,--whole-archive $(KRYON_LIB) -Wl,--no-whole-archive \
	$(BOX2D_A) $(LIBOQS_A) $(CURL_A) -lssl -lcrypto \
	$(CMARK_EXT_A) $(CMARK_A) $(CURL_CODEC_LDLIBS) -lz
CPPFLAGS += $(GTK_PKG_CFLAGS)
CPPFLAGS += $(X11_PKG_CFLAGS)

ifeq ($(KRYON_BACKEND),libdraw)
  CPPFLAGS += -DKRYON_BACKEND_LIBDRAW -I$(PLAN9PORT_DIR)/include
  LDLIBS += -L$(PLAN9PORT_DIR)/lib -ldraw -lmemdraw -lmux -lthread -l9
endif

LDLIBS += $(GTK_PKG_LIBS) $(PLATFORM_LDLIBS) -lpthread -lm

BUILD_DIR := build/$(PLATFORM)-$(ARCH)
BIN := $(BUILD_DIR)/$(APP_NAME)
TEST_BIN := $(BUILD_DIR)/rill_shell_test
LINUX_LAUNCHER_TEST_BIN := $(BUILD_DIR)/rill_linux_launcher_test
SRCS := src/main.c src/rill_settings.c src/rill_shell.c src/rill_panel.c src/rill_x11.c $(PLATFORM_SRC) $(WAYLAND_SRC)
TEST_SRCS := tests/rill_shell_test.c src/rill_shell.c src/rill_panel.c src/platform_stub.c
LINUX_LAUNCHER_TEST_SRCS := tests/rill_linux_launcher_test.c src/platform_linux.c src/rill_wayland.c $(WAYLAND_SRC)

.PHONY: all clean run test visual-test windowed-smoke kryon FORCE

all: $(BIN)

kryon: $(KRYON_LIB)

$(KRYON_LIB): FORCE
	$(MAKE) -C $(KRYON_DIR) KRYON_BACKEND=$(KRYON_BACKEND) \
		BUILD_ROOT=$(KRYON_BUILD_ROOT) $(KRYON_LIB)

$(BUILD_DIR):
	mkdir -p $@

$(BIN): $(wildcard include/*.h) $(SRCS) $(KRYON_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS) $(LDLIBS)

$(TEST_BIN): $(wildcard include/*.h) $(TEST_SRCS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(TEST_SRCS)

$(LINUX_LAUNCHER_TEST_BIN): $(wildcard include/*.h) $(LINUX_LAUNCHER_TEST_SRCS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(LINUX_LAUNCHER_TEST_SRCS) \
		$(GTK_PKG_LIBS) $(PLATFORM_LDLIBS) $(shell pkg-config --libs wayland-client 2>/dev/null)

ifeq ($(PLATFORM),linux)
test: $(TEST_BIN) $(LINUX_LAUNCHER_TEST_BIN)
	$(TEST_BIN)
	$(LINUX_LAUNCHER_TEST_BIN)
else
test: $(TEST_BIN)
	$(TEST_BIN)
endif

visual-test: $(BIN)
	PLAN9PORT_DIR="$(PLAN9PORT_DIR)" RILL_BIN="$(abspath $(BIN))" \
		sh tests/rill_visual_test.sh

windowed-smoke: $(BIN)
	PLAN9PORT_DIR="$(abspath $(PLAN9PORT_DIR))" RILL_BIN="$(abspath $(BIN))" \
		sh tests/rill_windowed_smoke.sh

run: $(BIN)
	env PLAN9="$(PLAN9PORT_DIR)" PATH="$(PLAN9PORT_DIR)/bin:$(PATH)" \
		DEVDRAW="$(PLAN9PORT_DIR)/bin/devdraw" $(BIN)

clean:
	rm -rf build

$(BUILD_DIR)/rill_platform_test: tests/rill_platform_test.c src/platform_plan9.c src/rill_panel.c src/rill_settings.c $(wildcard include/*.h) | $(BUILD_DIR)
	$(CC) -Iinclude $(CFLAGS) -o $@ tests/rill_platform_test.c src/platform_plan9.c src/rill_panel.c src/rill_settings.c

$(BUILD_DIR)/rill_x11_protocol_test: tests/rill_x11_protocol_test.c src/rill_x11.c $(wildcard include/*.h) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ tests/rill_x11_protocol_test.c src/rill_x11.c $(PLATFORM_LDLIBS)

.PHONY: platform-test protocol-test
platform-test: $(BUILD_DIR)/rill_platform_test
	$(BUILD_DIR)/rill_platform_test

protocol-test: $(BUILD_DIR)/rill_x11_protocol_test
	xvfb-run -a $(BUILD_DIR)/rill_x11_protocol_test

test: platform-test

build/protocols/toplevel-client.h: protocols/wlr-foreign-toplevel-management-unstable-v1.xml
	mkdir -p build/protocols
	wayland-scanner client-header $< $@

build/protocols/toplevel-protocol.c: protocols/wlr-foreign-toplevel-management-unstable-v1.xml build/protocols/toplevel-client.h
	wayland-scanner private-code $< $@

build/protocols/toplevel-server.h: protocols/wlr-foreign-toplevel-management-unstable-v1.xml
	mkdir -p build/protocols
	wayland-scanner server-header $< $@

$(BUILD_DIR)/rill_wayland_test: tests/rill_wayland_test.c src/rill_wayland.c build/protocols/toplevel-server.h build/protocols/toplevel-protocol.c $(wildcard include/*.h) | $(BUILD_DIR)
	$(CC) -Iinclude -Ibuild/protocols -DRILL_HAS_WAYLAND $(CFLAGS) -o $@ tests/rill_wayland_test.c src/rill_wayland.c build/protocols/toplevel-protocol.c $(shell pkg-config --cflags --libs wayland-client wayland-server)

.PHONY: wayland-test
wayland-test: $(BUILD_DIR)/rill_wayland_test
	$(BUILD_DIR)/rill_wayland_test

.PHONY: xfce-smoke
xfce-smoke: $(BIN)
	PLAN9PORT_DIR="$(abspath $(PLAN9PORT_DIR))" RILL_BIN="$(abspath $(BIN))" sh tests/rill_xfce_smoke.sh

PREFIX ?= /usr/local
.PHONY: install session-test
install: $(BIN)
	install -Dm755 $(WM_BIN) $(DESTDIR)$(PREFIX)/bin/rill-wm
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/rill
	install -Dm755 scripts/rill-session $(DESTDIR)$(PREFIX)/bin/rill-session
	install -Dm755 scripts/rill-window $(DESTDIR)$(PREFIX)/bin/rill-window
	install -Dm644 session/rill.desktop $(DESTDIR)$(PREFIX)/share/xsessions/rill.desktop

session-test:
	python3 tests/session_test.py

test: session-test

.PHONY: session-smoke
session-smoke: $(BIN)
	xvfb-run -a -s '-screen 0 1280x800x24' python3 tests/session_smoke.py

.PHONY: plugin-smoke
plugin-smoke: $(BIN)
	RILL_TEST_ALL_PLUGINS=1 xvfb-run -a -s '-screen 0 1280x800x24' python3 tests/session_smoke.py

.PHONY: nested-smoke
nested-smoke: $(BIN)
	RILL_TEST_NESTED=1 xvfb-run -a -s '-screen 0 1440x1000x24' python3 tests/session_smoke.py

WM_PKGS := x11 xext xft xrandr xrender xcomposite xdamage xfixes sm ice
WM_BIN := $(BUILD_DIR)/rill-wm
$(WM_BIN): src/wm.c src/wm_compositor.c src/session.c include/session.h include/wm_compositor.h | $(BUILD_DIR)
	$(CC) -Iinclude $(CFLAGS) $(shell pkg-config --cflags $(WM_PKGS)) -o $@ src/wm.c src/wm_compositor.c src/session.c $(shell pkg-config --libs $(WM_PKGS))

all: $(WM_BIN)
install session-smoke plugin-smoke nested-smoke: $(WM_BIN)

$(BUILD_DIR)/wm_test: tests/wm_test.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $< $(shell pkg-config --cflags --libs x11 xtst xcomposite xext)
.PHONY: wm-test
wm-test: $(WM_BIN) $(BUILD_DIR)/wm_test
	xvfb-run -a -s '-screen 0 1280x800x24' $(BUILD_DIR)/wm_test $(abspath $(WM_BIN))
