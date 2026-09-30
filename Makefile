APP_NAME := rill
.DEFAULT_GOAL := all
KRYON_DIR ?= ../kryon
PLAN9PORT_DIR ?= $(if $(wildcard ../plan9port/lib/libdraw.a),../plan9port,../../taijiosnet/plan9port)
KRYON_BACKEND ?= libdraw
KRYON_WITH_SYNC ?= 0
ZIRAN_DIR ?= ../../ziranlang/ziran
ZIRAN ?= $(ZIRAN_DIR)/build/bin/ziran

SHELL_MODULES := native_memory platform_types shell_types shell run_dialog applications clock date_time_types date_time_linux calendar
SHELL_GEN := build/ziran/c
SHELL_C := $(addprefix $(SHELL_GEN)/,$(addsuffix .c,$(SHELL_MODULES)))
PERSISTENCE_MODULES := c_string file_linux native_files panel_types panel settings
PERSISTENCE_C := $(addprefix $(SHELL_GEN)/,$(addsuffix .c,$(PERSISTENCE_MODULES)))
STUB_C := $(SHELL_GEN)/platform_stub.c
PLAN9_HOST_GEN := build/ziran/platform-c
PLAN9_C := $(PLAN9_HOST_GEN)/platform_plan9.c
SHELL_STAMP := $(SHELL_GEN)/.generated
SHELL_SOURCES := $(wildcard src/*.zi $(ZIRAN_DIR)/std/*.zi)
PERSISTENCE_SOURCES := src/native_files.zi src/panel_types.zi src/panel.zi src/settings.zi \
	$(ZIRAN_DIR)/std/c_string.zi $(ZIRAN_DIR)/std/file_linux.zi $(ZIRAN_DIR)/std/file_plan9.zi

$(SHELL_STAMP): $(SHELL_SOURCES) $(PERSISTENCE_SOURCES) $(ZIRAN)
	env -u DISPLAY -u WAYLAND_DISPLAY $(ZIRAN) build --target=c --root src --module-path $(ZIRAN_DIR)/std -o $(SHELL_GEN) src/shell.zi src/platform_stub.zi src/panel.zi src/settings.zi src/run_dialog.zi src/applications.zi src/clock.zi
	touch $@

$(SHELL_C) $(PERSISTENCE_C) $(STUB_C): $(SHELL_STAMP)

$(PLAN9_C): src/platform_plan9.zi src/platform_types.zi src/native_memory.zi $(PERSISTENCE_SOURCES) $(ZIRAN)
	env -u DISPLAY -u WAYLAND_DISPLAY $(ZIRAN) build --target=c --root src --module-path $(ZIRAN_DIR)/std -o $(PLAN9_HOST_GEN) src/platform_plan9.zi

.PHONY: ziran-c-plan9 shell-test persistence-test
ziran-c-plan9: run-dialog-plan9 applications-plan9 calendar-plan9
	env -u DISPLAY -u WAYLAND_DISPLAY $(ZIRAN) build --target=plan9-c --define NATIVE_PLAN9 --root src --module-path $(ZIRAN_DIR)/std -o build/ziran/plan9 src/shell.zi src/panel.zi src/settings.zi src/platform_plan9.zi src/run_dialog.zi src/applications.zi src/clock.zi
	env -u DISPLAY -u WAYLAND_DISPLAY $(ZIRAN) build --target=plan9-c --define NATIVE_PLAN9 --root tests --module-path src --module-path $(ZIRAN_DIR)/std -o build/ziran/plan9-test tests/persistence_test.zi

shell-test:
	ZIRAN="$(abspath $(ZIRAN))" sh tests/shell_test.sh

persistence-test:
	ZIRAN="$(abspath $(ZIRAN))" ZIRAN_STD="$(abspath $(ZIRAN_DIR)/std)" sh tests/persistence_test.sh

.PHONY: run-test
run-test:
	ZIRAN="$(abspath $(ZIRAN))" ZIRAN_STD="$(abspath $(ZIRAN_DIR)/std)" sh tests/run_test.sh

# Current Kryon UI entrypoint. Linux services remain C until their conversion;
# the native Plan 9 entrypoint and services are entirely current Ziran.
RUN_GEN := build/ziran/run-c
RUN_BIN := build/rill-run
RUN_SOURCES := $(wildcard app/*.zi src/*.zi $(KRYON_DIR)/src/ui/*.zi $(KRYON_DIR)/src/ui/*/*.zi $(KRYON_DIR)/src/backend/*.zi $(ZIRAN_DIR)/std/*.zi)

.PHONY: run-dialog-build run-dialog-plan9 run-ui-test run-window-test
run-ui-test:
	ZIRAN="$(abspath $(ZIRAN))" ZIRAN_STD="$(abspath $(ZIRAN_DIR)/std)" KRYON_DIR="$(abspath $(KRYON_DIR))" sh tests/run_ui_test.sh

run-dialog-plan9:
	env -u DISPLAY -u WAYLAND_DISPLAY $(ZIRAN) build --target=plan9-c --define NATIVE_PLAN9 --define PLAN9_BUILD --root app --module-path src --module-path $(KRYON_DIR)/src/ui --module-path $(KRYON_DIR)/src/backend --module-path $(ZIRAN_DIR)/std -o build/ziran/run-plan9 app/run_main.zi

$(RUN_GEN)/.generated: $(RUN_SOURCES) $(ZIRAN)
	env -u DISPLAY -u WAYLAND_DISPLAY $(ZIRAN) build --target=c --root app --module-path src --module-path $(KRYON_DIR)/src/ui --module-path $(KRYON_DIR)/src/backend --module-path $(ZIRAN_DIR)/std -o $(RUN_GEN) app/run_main.zi
	touch $@

run-dialog-build: $(RUN_BIN)

run-window-test: $(RUN_BIN)
	env -u DISPLAY -u WAYLAND_DISPLAY -u XAUTHORITY RILL_PRIVATE_XVFB=1 \
		PLAN9="$(abspath $(PLAN9PORT_DIR))" DEVDRAW="$(abspath $(PLAN9PORT_DIR))/bin/devdraw" \
		xvfb-run -a -n 100 python3 tests/run_window_test.py

APPLICATIONS_GEN := build/ziran/applications-c
APPLICATIONS_BIN := build/rill-applications

.PHONY: applications-build applications-plan9 applications-test applications-ui-test applications-window-test
applications-build: $(APPLICATIONS_BIN)

applications-plan9:
	env -u DISPLAY -u WAYLAND_DISPLAY $(ZIRAN) build --target=plan9-c --define NATIVE_PLAN9 --define PLAN9_BUILD --root app --module-path src --module-path $(KRYON_DIR)/src/ui --module-path $(KRYON_DIR)/src/backend --module-path $(ZIRAN_DIR)/std -o build/ziran/applications-plan9 app/applications_main.zi

$(APPLICATIONS_GEN)/.generated: $(RUN_SOURCES) $(ZIRAN)
	env -u DISPLAY -u WAYLAND_DISPLAY $(ZIRAN) build --target=c --root app --module-path src --module-path $(KRYON_DIR)/src/ui --module-path $(KRYON_DIR)/src/backend --module-path $(ZIRAN_DIR)/std -o $(APPLICATIONS_GEN) app/applications_main.zi
	touch $@

applications-test:
	ZIRAN="$(abspath $(ZIRAN))" ZIRAN_STD="$(abspath $(ZIRAN_DIR)/std)" sh tests/applications_test.sh

applications-ui-test:
	ZIRAN="$(abspath $(ZIRAN))" ZIRAN_STD="$(abspath $(ZIRAN_DIR)/std)" KRYON_DIR="$(abspath $(KRYON_DIR))" sh tests/applications_ui_test.sh

applications-window-test: $(APPLICATIONS_BIN)
	env -u DISPLAY -u WAYLAND_DISPLAY -u XAUTHORITY RILL_PRIVATE_XVFB=1 \
		PLAN9="$(abspath $(PLAN9PORT_DIR))" DEVDRAW="$(abspath $(PLAN9PORT_DIR))/bin/devdraw" \
		xvfb-run -a -n 100 python3 tests/applications_window_test.py

CALENDAR_GEN := build/ziran/calendar-c
CALENDAR_BIN := build/rill-calendar

.PHONY: calendar-build calendar-plan9 clock-test calendar-ui-test calendar-window-test
calendar-build: $(CALENDAR_BIN)

calendar-plan9:
	env -u DISPLAY -u WAYLAND_DISPLAY $(ZIRAN) build --target=plan9-c --define NATIVE_PLAN9 --define PLAN9_BUILD --root app --module-path src --module-path $(KRYON_DIR)/src/ui --module-path $(KRYON_DIR)/src/backend --module-path $(ZIRAN_DIR)/std -o build/ziran/calendar-plan9 app/calendar_main.zi

$(CALENDAR_GEN)/.generated: $(RUN_SOURCES) $(ZIRAN)
	env -u DISPLAY -u WAYLAND_DISPLAY $(ZIRAN) build --target=c --root app --module-path src --module-path $(KRYON_DIR)/src/ui --module-path $(KRYON_DIR)/src/backend --module-path $(ZIRAN_DIR)/std -o $(CALENDAR_GEN) app/calendar_main.zi
	touch $@

clock-test:
	ZIRAN="$(abspath $(ZIRAN))" ZIRAN_STD="$(abspath $(ZIRAN_DIR)/std)" sh tests/clock_test.sh

calendar-ui-test:
	ZIRAN="$(abspath $(ZIRAN))" ZIRAN_STD="$(abspath $(ZIRAN_DIR)/std)" KRYON_DIR="$(abspath $(KRYON_DIR))" sh tests/calendar_ui_test.sh

calendar-window-test: $(CALENDAR_BIN)
	env -u DISPLAY -u WAYLAND_DISPLAY -u XAUTHORITY RILL_PRIVATE_XVFB=1 \
		PLAN9="$(abspath $(PLAN9PORT_DIR))" DEVDRAW="$(abspath $(PLAN9PORT_DIR))/bin/devdraw" \
		xvfb-run -a -n 100 python3 tests/calendar_window_test.py

CC ?= cc
CFLAGS ?= -Wall -Wextra -O2
CPPFLAGS := -Iinclude -I$(KRYON_DIR)/include
LDFLAGS := -rdynamic
LDLIBS =
GTK_PKG_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 gio-unix-2.0 2>/dev/null)
GTK_PKG_LIBS := $(shell pkg-config --libs gtk+-3.0 gio-unix-2.0 2>/dev/null)
X11_PKGS := x11 xext xrandr xcomposite xdamage xfixes xrender xtst
X11_PKG_CFLAGS := $(shell pkg-config --cflags $(X11_PKGS) 2>/dev/null)
X11_PKG_LIBS := $(shell pkg-config --libs $(X11_PKGS) 2>/dev/null || printf '%s' '-lX11 -lXext -lXrandr -lXcomposite -lXdamage -lXfixes -lXrender -lXtst')

UNAME_S := $(shell uname -s 2>/dev/null)
UNAME_M := $(shell uname -m 2>/dev/null)
ifeq ($(UNAME_M),amd64)
  ARCH := x86_64
else
  ARCH := $(UNAME_M)
endif
ifeq ($(UNAME_S),Linux)
  PLATFORM := linux
  PLATFORM_SRC := src/platform_linux.c src/files.c src/rill_wayland.c src/session.c
  ifneq ($(shell pkg-config --exists wayland-client && command -v wayland-scanner),)
    WAYLAND_SRC := build/protocols/toplevel-protocol.c
    CPPFLAGS += -DRILL_HAS_WAYLAND -Ibuild/protocols $(shell pkg-config --cflags wayland-client)
    LDLIBS += $(shell pkg-config --libs wayland-client)
  endif
  PLATFORM_LDLIBS := $(X11_PKG_LIBS) $(shell pkg-config --libs sm ice) -ldl -lrt
else
  PLATFORM := unknown
  PLATFORM_SRC := $(STUB_C)
  PLATFORM_LDLIBS :=
endif

$(RUN_BIN): $(RUN_GEN)/.generated src/platform_linux.c src/files.c src/rill_wayland.c $(WAYLAND_SRC) $(wildcard include/*.h)
	$(CC) $(CPPFLAGS) -I$(RUN_GEN) $(CFLAGS) \
		-ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $(RUN_GEN)/*.c \
		src/platform_linux.c src/files.c src/rill_wayland.c $(WAYLAND_SRC) $(GTK_PKG_LIBS) $(PLATFORM_LDLIBS) \
		$(shell pkg-config --libs wayland-client 2>/dev/null) \
		-Wl,-E -L$(PLAN9PORT_DIR)/lib -ldraw -lmemdraw -lmux -lthread -l9 -lpthread -lm -lcairo

$(APPLICATIONS_BIN): $(APPLICATIONS_GEN)/.generated src/platform_linux.c src/files.c src/rill_wayland.c $(WAYLAND_SRC) $(wildcard include/*.h)
	$(CC) $(CPPFLAGS) -I$(APPLICATIONS_GEN) $(CFLAGS) \
		-ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $(APPLICATIONS_GEN)/*.c \
		src/platform_linux.c src/files.c src/rill_wayland.c $(WAYLAND_SRC) $(GTK_PKG_LIBS) $(PLATFORM_LDLIBS) \
		$(shell pkg-config --libs wayland-client 2>/dev/null) \
		-Wl,-E -L$(PLAN9PORT_DIR)/lib -ldraw -lmemdraw -lmux -lthread -l9 -lpthread -lm -lcairo

$(CALENDAR_BIN): $(CALENDAR_GEN)/.generated src/platform_linux.c src/files.c src/rill_wayland.c $(WAYLAND_SRC) $(wildcard include/*.h)
	$(CC) $(CPPFLAGS) -I$(CALENDAR_GEN) $(CFLAGS) \
		-ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $(CALENDAR_GEN)/*.c \
		src/platform_linux.c src/files.c src/rill_wayland.c $(WAYLAND_SRC) $(GTK_PKG_LIBS) $(PLATFORM_LDLIBS) \
		$(shell pkg-config --libs wayland-client 2>/dev/null) \
		-Wl,-E -L$(PLAN9PORT_DIR)/lib -ldraw -lmemdraw -lmux -lthread -l9 -lpthread -lm -lcairo

KRYON_BUILD_ROOT := $(abspath build/kryon-$(KRYON_BACKEND))
KRYON_BUILD_DIR := $(KRYON_BUILD_ROOT)/$(PLATFORM)-$(ARCH)
KRYON_LIB := $(KRYON_BUILD_DIR)/libkryon.a
CPPFLAGS += -I$(KRYON_BUILD_DIR)/generated/include \
	-I$(KRYON_BUILD_DIR)/generated/src -DKRYON_WITH_SYNC=$(KRYON_WITH_SYNC)
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
	$(BOX2D_A) $(CURL_A) -lssl -lcrypto \
	$(CMARK_EXT_A) $(CMARK_A) $(CURL_CODEC_LDLIBS) -lz
ifeq ($(KRYON_WITH_SYNC),1)
  LDLIBS += $(LIBOQS_A)
endif
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
SRCS := src/main.c $(SHELL_C) $(PERSISTENCE_C) src/rill_x11.c src/rill_dnd.c $(PLATFORM_SRC) $(WAYLAND_SRC)
TEST_SRCS := tests/rill_shell_test.c $(SHELL_C) $(PERSISTENCE_C) $(STUB_C)
LINUX_LAUNCHER_TEST_SRCS := tests/rill_linux_launcher_test.c src/platform_linux.c src/files.c src/rill_wayland.c $(WAYLAND_SRC)
DND_TEST_SRCS := tests/rill_dnd_test.c src/rill_dnd.c

.PHONY: all clean run test clean-text-api-check visual-test windowed-smoke kryon FORCE

all: $(BIN)

kryon: $(KRYON_LIB)

clean-text-api-check:
	python3 $(KRYON_DIR)/scripts/check-clean-text-api.py src include tests

test: clean-text-api-check

$(KRYON_LIB): FORCE
	$(MAKE) -C $(KRYON_DIR) KRYON_BACKEND=$(KRYON_BACKEND) \
		KRYON_WITH_SYNC=$(KRYON_WITH_SYNC) PLAN9PORT_DIR=$(abspath $(PLAN9PORT_DIR)) \
		BUILD_ROOT=$(KRYON_BUILD_ROOT) $(KRYON_LIB)

$(BUILD_DIR):
	mkdir -p $@

$(BIN): $(wildcard include/*.h) $(SRCS) $(KRYON_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS) $(LDLIBS)

$(TEST_BIN): $(wildcard include/*.h) $(TEST_SRCS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(TEST_SRCS)

$(LINUX_LAUNCHER_TEST_BIN): $(wildcard include/*.h) $(LINUX_LAUNCHER_TEST_SRCS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DRILL_DBUS_MOCK='"$(abspath tests/rill_dbus_mock.py)"' -o $@ $(LINUX_LAUNCHER_TEST_SRCS) \
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

$(BUILD_DIR)/rill_platform_test: tests/rill_platform_test.c $(PLAN9_C) $(SHELL_GEN)/native_memory.c $(SHELL_GEN)/platform_types.c $(PERSISTENCE_C) $(wildcard include/*.h) | $(BUILD_DIR)
	$(CC) -Iinclude $(CFLAGS) -o $@ tests/rill_platform_test.c $(PLAN9_C) $(SHELL_GEN)/native_memory.c $(SHELL_GEN)/platform_types.c $(PERSISTENCE_C)

$(BUILD_DIR)/rill_x11_protocol_test: tests/rill_x11_protocol_test.c src/rill_x11.c $(wildcard include/*.h) $(KRYON_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ tests/rill_x11_protocol_test.c src/rill_x11.c $(PLATFORM_LDLIBS)

$(BUILD_DIR)/rill_xembed_test: tests/rill_xembed_test.c src/platform_linux.c src/files.c src/rill_wayland.c $(wildcard include/*.h) $(WAYLAND_SRC) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/rill_xembed_test.c src/platform_linux.c src/files.c src/rill_wayland.c $(WAYLAND_SRC) \
		$(GTK_PKG_LIBS) $(PLATFORM_LDLIBS) $(shell pkg-config --libs wayland-client 2>/dev/null)

$(BUILD_DIR)/rill_xsettings_test: tests/rill_xsettings_test.c src/platform_linux.c src/files.c src/rill_wayland.c $(wildcard include/*.h) $(WAYLAND_SRC) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/rill_xsettings_test.c src/platform_linux.c src/files.c src/rill_wayland.c $(WAYLAND_SRC) \
		$(GTK_PKG_LIBS) $(PLATFORM_LDLIBS) $(shell pkg-config --libs wayland-client 2>/dev/null)

$(BUILD_DIR)/rill_clipboard_test: tests/rill_clipboard_test.c src/platform_linux.c src/files.c src/rill_wayland.c $(wildcard include/*.h) $(WAYLAND_SRC) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/rill_clipboard_test.c src/platform_linux.c src/files.c src/rill_wayland.c $(WAYLAND_SRC) \
		$(GTK_PKG_LIBS) $(PLATFORM_LDLIBS) $(shell pkg-config --libs wayland-client 2>/dev/null)

.PHONY: platform-test protocol-test xembed-test xsettings-test clipboard-test
platform-test: $(BUILD_DIR)/rill_platform_test
	env -u DISPLAY -u WAYLAND_DISPLAY $(BUILD_DIR)/rill_platform_test
	ZIRAN="$(abspath $(ZIRAN))" ZIRAN_STD="$(abspath $(ZIRAN_DIR)/std)" sh tests/platform_plan9_test.sh

protocol-test: $(BUILD_DIR)/rill_x11_protocol_test
	xvfb-run -a $(BUILD_DIR)/rill_x11_protocol_test

xembed-test: $(BUILD_DIR)/rill_xembed_test
	xvfb-run -a $(BUILD_DIR)/rill_xembed_test

xsettings-test: $(BUILD_DIR)/rill_xsettings_test
	xvfb-run -a $(BUILD_DIR)/rill_xsettings_test

clipboard-test: $(BUILD_DIR)/rill_clipboard_test
	xvfb-run -a $(BUILD_DIR)/rill_clipboard_test

$(BUILD_DIR)/rill_dnd_test: $(wildcard include/rill_dnd.h) $(DND_TEST_SRCS) | $(BUILD_DIR)
	$(CC) -Iinclude $(X11_PKG_CFLAGS) $(CFLAGS) -o $@ $(DND_TEST_SRCS) $(X11_PKG_LIBS)

.PHONY: dnd-test
dnd-test: $(BUILD_DIR)/rill_dnd_test
	xvfb-run -a $(BUILD_DIR)/rill_dnd_test

test: shell-test persistence-test platform-test xembed-test xsettings-test clipboard-test sessiond-test dnd-test

$(BUILD_DIR)/files_test: tests/files_test.c src/files.c include/files.h | $(BUILD_DIR)
	$(CC) -Iinclude $(GTK_PKG_CFLAGS) $(CFLAGS) -o $@ tests/files_test.c src/files.c $(GTK_PKG_LIBS) -lX11

.PHONY: files-test
files-test: $(BUILD_DIR)/files_test
	xvfb-run -a $(BUILD_DIR)/files_test

test: files-test

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
.PHONY: install run-dialog-install applications-install calendar-install session-test
install: $(BIN) $(RUN_BIN) $(APPLICATIONS_BIN) $(CALENDAR_BIN)
	install -Dm755 $(WM_BIN) $(DESTDIR)$(PREFIX)/bin/rill-wm
	install -Dm755 $(SESSIOND_BIN) $(DESTDIR)$(PREFIX)/bin/rill-sessiond
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/rill
	install -Dm755 $(RUN_BIN) $(DESTDIR)$(PREFIX)/bin/rill-run
	install -Dm755 $(APPLICATIONS_BIN) $(DESTDIR)$(PREFIX)/bin/rill-applications
	install -Dm755 $(CALENDAR_BIN) $(DESTDIR)$(PREFIX)/bin/rill-calendar
	install -Dm755 scripts/rill-session $(DESTDIR)$(PREFIX)/bin/rill-session
	install -Dm755 scripts/rill-window $(DESTDIR)$(PREFIX)/bin/rill-window
	install -Dm644 session/rill.desktop $(DESTDIR)$(PREFIX)/share/xsessions/rill.desktop
	install -Dm644 session/rill-xfce.desktop $(DESTDIR)$(PREFIX)/share/xsessions/rill-xfce.desktop
	install -Dm644 session/applications/rill-settings.desktop $(DESTDIR)$(PREFIX)/share/applications/rill-settings.desktop
	install -Dm644 session/applications/rill-run.desktop $(DESTDIR)$(PREFIX)/share/applications/rill-run.desktop
	install -Dm644 session/applications/rill-applications.desktop $(DESTDIR)$(PREFIX)/share/applications/rill-applications.desktop
	install -Dm644 session/applications/rill-calendar.desktop $(DESTDIR)$(PREFIX)/share/applications/rill-calendar.desktop

calendar-install: $(CALENDAR_BIN)
	install -Dm755 $(CALENDAR_BIN) $(DESTDIR)$(PREFIX)/bin/rill-calendar
	install -Dm644 session/applications/rill-calendar.desktop $(DESTDIR)$(PREFIX)/share/applications/rill-calendar.desktop

applications-install: $(APPLICATIONS_BIN)
	install -Dm755 $(APPLICATIONS_BIN) $(DESTDIR)$(PREFIX)/bin/rill-applications
	install -Dm644 session/applications/rill-applications.desktop $(DESTDIR)$(PREFIX)/share/applications/rill-applications.desktop

run-dialog-install: $(RUN_BIN)
	install -Dm755 $(RUN_BIN) $(DESTDIR)$(PREFIX)/bin/rill-run
	install -Dm644 session/applications/rill-run.desktop $(DESTDIR)$(PREFIX)/share/applications/rill-run.desktop

session-test:
	python3 tests/session_test.py

test: session-test

.PHONY: session-smoke
session-smoke: $(BIN)
	xvfb-run -a -s '-screen 0 1280x800x24' python3 tests/session_smoke.py

.PHONY: native-session-smoke
native-session-smoke: $(BIN) $(WM_BIN) $(SESSIOND_BIN)
	RILL_TEST_NATIVE=1 xvfb-run -a -s '-screen 0 1280x800x24' python3 tests/session_smoke.py

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

SESSIOND_BIN := $(BUILD_DIR)/rill-sessiond
$(SESSIOND_BIN): src/rill_sessiond.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(X11_PKG_CFLAGS) $(shell pkg-config --cflags sm ice gio-unix-2.0) -o $@ src/rill_sessiond.c $(shell pkg-config --libs sm ice gio-unix-2.0) $(X11_PKG_LIBS)

$(BUILD_DIR)/rill_sessiond_test: tests/rill_sessiond_test.c $(SESSIOND_BIN) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(shell pkg-config --cflags sm ice) -o $@ tests/rill_sessiond_test.c $(shell pkg-config --libs sm ice)

.PHONY: sessiond-test session-lifecycle-test
sessiond-test: $(BUILD_DIR)/rill_sessiond_test $(SESSIOND_BIN)
	$(BUILD_DIR)/rill_sessiond_test $(abspath $(SESSIOND_BIN))

session-lifecycle-test: $(SESSIOND_BIN)
	python3 tests/session_lifecycle_test.py

test: session-lifecycle-test

all: $(WM_BIN) $(SESSIOND_BIN)
install session-smoke native-session-smoke plugin-smoke nested-smoke: $(SESSIOND_BIN)
install session-smoke native-session-smoke plugin-smoke nested-smoke: $(WM_BIN)

$(BUILD_DIR)/wm_test: tests/wm_test.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $< $(shell pkg-config --cflags --libs x11 xtst xcomposite xext xrandr)
.PHONY: wm-test
wm-test: $(WM_BIN) $(BUILD_DIR)/wm_test
	xvfb-run -a -s '-screen 0 1280x800x24' $(BUILD_DIR)/wm_test $(abspath $(WM_BIN))
