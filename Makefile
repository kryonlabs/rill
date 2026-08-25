APP_NAME := rill
KRYON_DIR ?= ../kryon
PLAN9PORT_DIR ?= ../plan9port
KRYON_BACKEND ?= libdraw
KAPSULE_DIR ?= vendor/kapsule

CC ?= cc
CFLAGS ?= -Wall -Wextra -O2
CPPFLAGS := -Iinclude -I$(KRYON_DIR)/include
LDFLAGS :=
LDLIBS =
GTK_PKG_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 2>/dev/null)
GTK_PKG_LIBS := $(shell pkg-config --libs gtk+-3.0 2>/dev/null)

UNAME_S := $(shell uname -s 2>/dev/null)
UNAME_M := $(shell uname -m 2>/dev/null)
ifeq ($(UNAME_M),amd64)
  ARCH := x86_64
else
  ARCH := $(UNAME_M)
endif
ifeq ($(UNAME_S),Linux)
  PLATFORM := linux
  PLATFORM_SRC := src/platform_linux.c
  PLATFORM_LDLIBS := -ldl -lrt
else
  PLATFORM := unknown
  PLATFORM_SRC := src/platform_stub.c
  PLATFORM_LDLIBS :=
endif

KRYON_BUILD_ROOT := $(abspath build/kryon-$(KRYON_BACKEND))
KRYON_BUILD_DIR := $(KRYON_BUILD_ROOT)/$(PLATFORM)-$(ARCH)
KRYON_LIB := $(KRYON_BUILD_DIR)/libkryon.a
KAPSULE_BUILD_ROOT := $(abspath build/kapsule-$(KRYON_BACKEND))
KAPSULE_BIN := $(KAPSULE_BUILD_ROOT)/$(PLATFORM)-$(ARCH)/bin/kapsule
KAPSULE_HOST_LIB := $(KAPSULE_BUILD_ROOT)/$(PLATFORM)-$(ARCH)/lib/libkapsule_host.a
LDLIBS += $(KAPSULE_HOST_LIB) -L$(KRYON_BUILD_DIR) -lkryon
CPPFLAGS += $(GTK_PKG_CFLAGS) -DRILL_KAPSULE_BIN=\"$(KAPSULE_BIN)\"

ifeq ($(KRYON_BACKEND),libdraw)
  CPPFLAGS += -DKRYON_BACKEND_LIBDRAW -I$(PLAN9PORT_DIR)/include
  LDLIBS += -L$(PLAN9PORT_DIR)/lib -ldraw -lmemdraw -lmux -lthread -l9
endif

LDLIBS += $(GTK_PKG_LIBS) $(PLATFORM_LDLIBS) -lpthread -lm

BUILD_DIR := build/$(PLATFORM)-$(ARCH)
BIN := $(BUILD_DIR)/$(APP_NAME)
TEST_BIN := $(BUILD_DIR)/rill_shell_test
SRCS := src/main.c src/rill_shell.c $(PLATFORM_SRC)
TEST_SRCS := tests/rill_shell_test.c src/rill_shell.c src/platform_stub.c

.PHONY: all clean run test kryon kapsule

all: $(BIN) $(KAPSULE_BIN) $(KAPSULE_HOST_LIB)

kryon:
	$(MAKE) -C $(KRYON_DIR) KRYON_BACKEND=$(KRYON_BACKEND) \
		BUILD_ROOT=$(KRYON_BUILD_ROOT) $(KRYON_LIB)

$(KRYON_LIB): kryon

kapsule: $(KAPSULE_BIN) $(KAPSULE_HOST_LIB)

$(KAPSULE_BIN) $(KAPSULE_HOST_LIB): $(KRYON_LIB)
	$(MAKE) -C $(KAPSULE_DIR) KRYON_BACKEND=$(KRYON_BACKEND) \
		ENGINE_DIR=$(abspath $(KRYON_DIR)) \
		ENGINE_BUILD_ROOT=$(KRYON_BUILD_ROOT) \
		BUILD_ROOT=$(KAPSULE_BUILD_ROOT) \
		PLAN9PORT_DIR=$(abspath $(PLAN9PORT_DIR))

$(BUILD_DIR):
	mkdir -p $@

$(BIN): $(SRCS) $(KRYON_LIB) $(KAPSULE_HOST_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS) $(LDLIBS)

$(TEST_BIN): $(TEST_SRCS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(TEST_SRCS)

test: $(TEST_BIN)
	$(TEST_BIN)

run: $(BIN)
	env PLAN9="$(PLAN9PORT_DIR)" PATH="$(PLAN9PORT_DIR)/bin:$(PATH)" \
		DEVDRAW="$(PLAN9PORT_DIR)/bin/devdraw" $(BIN)

clean:
	rm -rf build
