#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET      := PlayLogLimiter
TITLEID     := 4200000000504C54
BUILD       := build
SOURCES     := source
DATA        := data
INCLUDES    := include
CONFIG_JSON := PlayLogLimiter.json

ARCH    := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS  := -g -Wall -O2 -ffunction-sections \
           $(ARCH) $(DEFINES)
CFLAGS  += $(INCLUDE) -D__SWITCH__

PSM_HEADER := $(DEVKITPRO)/libnx/include/switch/services/psm.h
ifneq ($(shell grep -c psmGetBatteryChargeInfoFields $(PSM_HEADER) 2>/dev/null),0)
CFLAGS  += -DPLAYLOG_HAVE_PSM_CHARGE_INFO
endif

CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions
ASFLAGS  := -g $(ARCH)
LDFLAGS  = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
LIBS     := -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lnx
LIBDIRS  := $(PORTLIBS) $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)

export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                $(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
export LD := $(CC)
else
export LD := $(CXX)
endif

export OFILES_BIN := $(addsuffix .o,$(BINFILES))
export OFILES_SRC := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES     := $(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN := $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
export APP_JSON := $(TOPDIR)/$(CONFIG_JSON)

.PHONY: $(BUILD) clean all package

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

package: all
	@rm -rf package
	@mkdir -p package/atmosphere/contents/$(TITLEID)/flags
	@cp $(TARGET).nsp package/atmosphere/contents/$(TITLEID)/exefs.nsp
	@touch package/atmosphere/contents/$(TITLEID)/flags/boot2.flag
	@printf '{\n  "name": "PlayLog Limiter",\n  "tid": "%s",\n  "requires_reboot": true\n}\n' "$(TITLEID)" > package/atmosphere/contents/$(TITLEID)/toolbox.json

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nsp $(TARGET).nso $(TARGET).npdm $(TARGET).elf package

else

.PHONY: all

DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).nsp

$(OUTPUT).nsp: $(OUTPUT).nso $(OUTPUT).npdm
	@rm -rf exefs
	@mkdir -p exefs
	@cp $(OUTPUT).nso exefs/main
	@cp $(OUTPUT).npdm exefs/main.npdm
	@build_pfs0 exefs $@
	@echo built ... $(notdir $@)

$(OUTPUT).nso: $(OUTPUT).elf
	@elf2nso $< $@
	@echo built ... $(notdir $@)

$(OUTPUT).npdm: $(APP_JSON)
	@npdmtool $< $@
	@echo built ... $(notdir $@)

$(OUTPUT).elf: $(OFILES)

$(OFILES_SRC): $(HFILES_BIN)

%.bin.o %_bin.h: %.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

endif
