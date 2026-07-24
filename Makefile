PROJECT_ROOT := $(abspath $(CURDIR))
SDK_ROOT     := $(PROJECT_ROOT)/.deps/openfgpaSDK
FAKE08_ROOT  := $(PROJECT_ROOT)/.deps/fake-08
APP_DIR      := $(PROJECT_ROOT)/src/app
BUILD_DIR    := $(PROJECT_ROOT)/build
TEST_BIN     := $(BUILD_DIR)/tests/pico8pocket_tests
RUNTIME_TEST_BIN := $(BUILD_DIR)/tests/pico8pocket_runtime_tests
COMPAT_SCAN_BIN := $(BUILD_DIR)/tools/pico8pocket_compat_scan
BUNDLE_LOCAL_CARTS ?= 0
export BUNDLE_LOCAL_CARTS
Z8LUA_ROOT   := $(FAKE08_ROOT)/libs/z8lua
MINIZ_ROOT   := $(FAKE08_ROOT)/libs/miniz

include $(PROJECT_ROOT)/mk/z8lua.mk

.DEFAULT_GOAL := test

deps:
	@./scripts/fetch-deps.sh

$(TEST_BIN): tests/test_main.c src/app/system_input.c src/runtime/cart.c src/runtime/cart_png.c \
		src/runtime/display.c include/p8p/cart.h include/p8p/display.h \
		$(MINIZ_ROOT)/miniz.c
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -Wno-strict-prototypes -pedantic \
		-Iinclude -I$(MINIZ_ROOT) \
		tests/test_main.c src/app/system_input.c src/runtime/cart.c src/runtime/cart_png.c \
		src/runtime/display.c $(MINIZ_ROOT)/miniz.c -o $@

$(RUNTIME_TEST_BIN): tests/runtime_test.cpp src/runtime/runtime.cpp src/runtime/audio.c src/runtime/cart.c src/app/state_store.c src/app/settings.c \
		src/runtime/cart_png.c include/p8p/runtime.h include/p8p/cart.h \
		$(Z8LUA_SOURCES) $(MINIZ_ROOT)/miniz.c
	@mkdir -p $(dir $@)
	$(CXX) -x c++ -std=c++17 -O2 -fwrapv -Wall -Wextra \
		-Wno-deprecated-declarations -Wno-unused-function -Wno-unused-parameter \
		-Iinclude -I$(Z8LUA_ROOT) -I$(MINIZ_ROOT) \
		tests/runtime_test.cpp src/runtime/runtime.cpp src/runtime/audio.c src/runtime/cart.c src/app/state_store.c src/app/settings.c \
		src/runtime/cart_png.c $(Z8LUA_SOURCES) $(MINIZ_ROOT)/miniz.c -lm -o $@

$(COMPAT_SCAN_BIN): tools/compat_scan.cpp src/runtime/runtime.cpp src/runtime/audio.c src/runtime/cart.c \
		src/runtime/cart_png.c include/p8p/runtime.h include/p8p/cart.h \
		$(Z8LUA_SOURCES) $(MINIZ_ROOT)/miniz.c
	@mkdir -p $(dir $@)
	$(CXX) -x c++ -std=c++17 -O2 -fwrapv -DP8P_RUNTIME_DEBUG -Wall -Wextra \
		-Wno-deprecated-declarations -Wno-unused-function -Wno-unused-parameter \
		-Iinclude -I$(Z8LUA_ROOT) -I$(MINIZ_ROOT) \
		tools/compat_scan.cpp src/runtime/runtime.cpp src/runtime/audio.c src/runtime/cart.c \
		src/runtime/cart_png.c $(Z8LUA_SOURCES) $(MINIZ_ROOT)/miniz.c -lm -o $@

test: deps $(TEST_BIN) $(RUNTIME_TEST_BIN)
	$(TEST_BIN)
	$(RUNTIME_TEST_BIN)

compat-scan: deps $(COMPAT_SCAN_BIN)

pocket-elf: deps
	$(MAKE) -C $(APP_DIR) \
		PROJECT_ROOT=$(PROJECT_ROOT) \
		SDK_ROOT=$(SDK_ROOT) \
		FAKE08_ROOT=$(FAKE08_ROOT)

pocket: pocket-elf
	@./scripts/assemble-pocket.sh

validate: pocket
	@./scripts/validate-pocket.sh

package: validate
	@./scripts/package-pocket.sh

# Explicit opt-in for private test builds.  Public packages never include
# locally supplied cartridges.
package-local:
	$(MAKE) package BUNDLE_LOCAL_CARTS=1

clean:
	$(MAKE) -C $(APP_DIR) clean PROJECT_ROOT=$(PROJECT_ROOT) SDK_ROOT=$(SDK_ROOT) 2>/dev/null || true
	rm -rf $(BUILD_DIR) $(PROJECT_ROOT)/.obj

.PHONY: deps test compat-scan pocket-elf pocket validate package package-local clean
