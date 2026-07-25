APP          := pico8pocket
PROJECT_ROOT ?= $(abspath ../..)
SDK_ROOT     ?= $(PROJECT_ROOT)/.deps/openfgpaSDK
FAKE08_ROOT  ?= $(PROJECT_ROOT)/.deps/fake-08
SDK_DIR      := $(SDK_ROOT)/src/sdk
OBJ_DIR      := $(PROJECT_ROOT)/.obj/$(APP)
BUILD_DIR    := $(OBJ_DIR)
SDK_CONTAINER := $(PROJECT_ROOT)/tools/sdk-container.sh
Z8LUA_ROOT := $(FAKE08_ROOT)/libs/z8lua
Z8LUA_GENERATED_DIR := $(PROJECT_ROOT)/build/generated/z8lua

include $(PROJECT_ROOT)/mk/z8lua.mk

Z8LUA_GENERATED := $(addprefix $(Z8LUA_GENERATED_DIR)/,$(addsuffix .cpp,$(Z8LUA_NAMES)))

SRCS := \
	$(PROJECT_ROOT)/src/app/main.c \
	$(PROJECT_ROOT)/src/app/state_store.c \
	$(PROJECT_ROOT)/src/app/settings.c \
	$(PROJECT_ROOT)/src/app/menu.c \
	$(PROJECT_ROOT)/src/app/system_input.c \
	$(PROJECT_ROOT)/src/app/scheduler.c \
	$(PROJECT_ROOT)/src/runtime/cart.c \
	$(PROJECT_ROOT)/src/runtime/cart_png.c \
	$(PROJECT_ROOT)/src/runtime/audio.c \
	$(PROJECT_ROOT)/src/runtime/display.c \
	$(PROJECT_ROOT)/src/platform/openfpgaos/platform.c \
	$(FAKE08_ROOT)/libs/miniz/miniz.c
SRCS_CXX := \
	$(PROJECT_ROOT)/src/runtime/runtime.cpp \
	$(SDK_DIR)/of_cxxabi.cpp \
	$(Z8LUA_GENERATED)

CFLAGS += -D_GNU_SOURCE -std=c11 -I$(PROJECT_ROOT)/include -I$(FAKE08_ROOT)/libs/miniz
CXXFLAGS += -D_GNU_SOURCE -DLUA_USE_LONGJMP -std=c++17 \
	-include stdlib.h \
	-include $(PROJECT_ROOT)/include/p8p/vm_fasttext.h \
	-I$(PROJECT_ROOT)/include \
	-I$(Z8LUA_ROOT) \
	-I$(PROJECT_ROOT)/src/compat/cxx

# z8lua is the dominant cost on the Pocket's small RISC-V CPU.  The later
# -O3 intentionally overrides sdk.mk's -O2, and LTO lets the compiler fold
# the many tiny VM/fixed-point helpers across translation units.
CFLAGS += -O3 -flto -fomit-frame-pointer
CXXFLAGS += -O3 -flto -fomit-frame-pointer -fwrapv
LDFLAGS += -flto

.DEFAULT_GOAL := all

include $(SDK_DIR)/sdk.mk

$(Z8LUA_GENERATED_DIR)/%.cpp: $(Z8LUA_ROOT)/%.c
	@mkdir -p $(dir $@)
	cp $< $@

all: $(OBJ_DIR)/app.elf
	@$(SIZE) $< 2>/dev/null || true

clean: sdk-clean

.PHONY: all clean
