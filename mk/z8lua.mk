Z8LUA_ROOT ?= $(FAKE08_ROOT)/libs/z8lua

Z8LUA_NAMES := \
	eris \
	lapi lauxlib lbaselib lcode lcorolib lctype ldblib ldebug ldo ldump \
	lfunc lgc linit llex lmem lobject lopcodes lparser lpico8lib lstate \
	lstring lstrlib ltable ltablib ltm lundump lvm lzio

Z8LUA_SOURCES := $(addprefix $(Z8LUA_ROOT)/,$(addsuffix .c,$(Z8LUA_NAMES)))
