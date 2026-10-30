# Build only regular files; no installation or device access.
LVGL ?= ../../.tools/sources/lvgl-9.4.0
OUT ?= ../../.tools/build/panel-native
CC ?= cc
CFLAGS += -O2 -std=c11 -D_DEFAULT_SOURCE -DLV_CONF_INCLUDE_SIMPLE -I. -I$(LVGL) -ffunction-sections -fdata-sections
SOURCES := $(shell find $(LVGL)/src -name '*.c')
OBJECTS := $(patsubst $(LVGL)/%.c,$(OUT)/%.o,$(SOURCES))

all: $(OUT)/tablet-panel

$(OUT)/tablet-panel: $(OUT)/panel.o $(OUT)/protocol.o $(OUT)/cJSON.o $(OBJECTS)
	$(CC) $(LDFLAGS) $^ -lm -o $@

$(OUT)/panel.o: panel.c lv_conf.h protocol.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -Wall -Wextra -Werror -c $< -o $@

$(OUT)/protocol.o: protocol.c protocol.h vendor/cJSON.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -Wall -Wextra -Werror -c $< -o $@

$(OUT)/cJSON.o: vendor/cJSON.c vendor/cJSON.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -DCJSON_NESTING_LIMIT=8 -c $< -o $@

$(OUT)/%.o: $(LVGL)/%.c lv_conf.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: all
