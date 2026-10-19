#---------------------------------------------------------------------------------
# GuideWatch.ovl : Guide Watch, a plain-text game walkthrough overlay (libultrahand).
# Pattern copied from ppkantorski/Ultrahand-Overlay's Makefile, trimmed to what a plain
# libultrahand overlay needs (no launcher/widget/logging directives).
#
#   ./build.sh    builds the overlay inside devkitpro/devkita64 (runs this Makefile)
#   make test     host unit tests on the Mac (clang++, no devkitPro needed)
#---------------------------------------------------------------------------------
.SUFFIXES:

#---------------------------------------------------------------------------------
# Host tests for the pure headers source/match.hpp and source/text.hpp
#---------------------------------------------------------------------------------
ifeq ($(MAKECMDGOALS),test)

HOSTCXX ?= clang++
HOSTFLAGS := -std=c++20 -Wall -Wextra -Werror -O2

.PHONY: test
test:
	@mkdir -p build/host
	@$(HOSTCXX) $(HOSTFLAGS) -o build/host/match_test tests/match_test.cpp
	@$(HOSTCXX) $(HOSTFLAGS) -o build/host/text_test tests/text_test.cpp
	@./build/host/match_test
	@./build/host/text_test

else

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
# App metadata (APP_TITLE / APP_VERSION end up in the .nacp that Ultrahand lists)
#---------------------------------------------------------------------------------
APP_TITLE   := Guide Watch
APP_AUTHOR  := Ludovic Verbeeck
APP_VERSION := 0.4.0
TARGET      := GuideWatch
BUILD       := build
SOURCES     := source
INCLUDES    := source
NO_ICON     := 1

# Pulls libultrahand's common/, libultra/ and libtesla/ sources + includes in
include $(TOPDIR)/libultrahand/ultrahand.mk

#---------------------------------------------------------------------------------
# Code generation (same flags as Ultrahand-Overlay / the libultrahand example)
#---------------------------------------------------------------------------------
ARCH := -march=armv8-a+simd+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS := -g -Wall -Os -ffunction-sections -fdata-sections -flto \
          -fuse-linker-plugin -fomit-frame-pointer -finline-small-functions \
          -fno-strict-aliasing -frename-registers -falign-functions=16 \
          $(ARCH) $(DEFINES)

CFLAGS += $(INCLUDE) -D__SWITCH__ -DAPP_VERSION="\"$(APP_VERSION)\"" -D_FORTIFY_SOURCE=2

CXXFLAGS := $(CFLAGS) -std=c++26 -Wno-dangling-else -ffast-math -fno-finite-math-only \
            -fno-unwind-tables -fno-asynchronous-unwind-tables \
            -fno-exceptions -fno-rtti

ASFLAGS := $(ARCH)
LDFLAGS := -specs=$(DEVKITPRO)/libnx/switch.specs $(ARCH) -Wl,-Map,$(notdir $*.map) \
           -Wl,--as-needed -Wl,--gc-sections

# Only libnx is linked (as Status-Monitor-Overlay does). The curl/zlib/minizip
# HEADERS are still needed to compile libultra's download_funcs.cpp; the stock
# devkita64 image has them (minizip ships inside switch-zlib). LTO + gc-sections
# drop the unused download code, so nothing else needs linking.
LIBS := -lnx

# Parallel LTO jobs
NPROC := $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo 1)
CXXFLAGS += -flto=$(NPROC)
LDFLAGS  += -flto=$(NPROC)

LIBDIRS := $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
# Standard devkitPro two-pass build (outer pass sets up paths, inner pass in build/)
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
export LD := $(CXX)

export OFILES_SRC := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o)
export OFILES     := $(OFILES_SRC)

export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

# Overlays are NROs with a .nacp and no icon
export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@rm -fr $(BUILD) $(TARGET).ovl $(TARGET).nro $(TARGET).nacp $(TARGET).elf

else

DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).ovl

# elf -> nro (renamed .ovl), then append the 'ULTR' signature Ultrahand looks for
$(OUTPUT).ovl: $(OUTPUT).elf $(OUTPUT).nacp
	@elf2nro $< $@ $(NROFLAGS)
	@printf 'ULTR' >> $@
	@echo "built ... $(notdir $(OUTPUT).ovl) (ULTR signature added)"

$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif

endif  # host test / devkitPro build
