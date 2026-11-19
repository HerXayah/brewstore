# SPDX-License-Identifier: MIT
#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>devkitPro)
endif

include $(DEVKITPRO)/libounce/switch2_rules

#---------------------------------------------------------------------------------
TARGET		:=	brewstore
BUILD		:=	build
SOURCES		:=	source
DATA		:=	data
INCLUDES	:=	include
APP_TITLE	:=	brewstore
APP_AUTHOR	:=	Miella Selene / Xayah
APP_VERSION	:=	1.2.2

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	$(SWITCH2_CPU) -mtp=soft -fPIE
LD	:=	$(PREFIX)g++

CFLAGS	:=	-g -Wall -O2 -ffunction-sections \
			$(ARCH) $(DEFINES) -D_GNU_SOURCE
CFLAGS	+=	$(INCLUDE) -D__SWITCH__

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=$(LIBNX)/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS	:=	-lminizip -lz -llzma -lnx -lm

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:=	$(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SRC	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES	:=	$(OFILES_BIN) $(OFILES_SRC)
export HFILES	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: $(BUILD) clean all

#---------------------------------------------------------------------------------
all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

#---------------------------------------------------------------------------------
else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
all	:	$(OUTPUT).nro

$(OUTPUT).nro	:	$(OUTPUT).elf $(OUTPUT).nacp
	@elf2nro $< $@ --nacp=$(OUTPUT).nacp --icon=$(TOPDIR)/icon.jpg

$(OUTPUT).elf	:	$(OFILES)

#---------------------------------------------------------------------------------
%.o: %.png
	@echo $(notdir $<)
	$(SILENTCMD)bin2s -a $< | $(AS) -o $@

define bin2o
	bin2s -a $< | $(AS) -o $(@)
	echo "extern const u8" `(echo $(<F) | sed -e 's/^\([[:digit:]]\)\(.*\)$$/_\1\2/g' -e 's/[^A-Za-z0-9_]/_/g')`"[];" > `(echo $(<F) | sed -e 's/^\([[:digit:]]\)\(.*\)$$/_\1\2/g' -e 's/[^A-Za-z0-9_]/_/g')`.h
	echo "extern const u32" `(echo $(<F) | sed -e 's/^\([[:digit:]]\)\(.*\)$$/_\1\2/g' -e 's/[^A-Za-z0-9_]/_/g')`"_size;" >> `(echo $(<F) | sed -e 's/^\([[:digit:]]\)\(.*\)$$/_\1\2/g' -e 's/[^A-Za-z0-9_]/_/g')`.h
endef

$(OFILES_BIN): %$(OFILES_BIN_SUFFIX): %
	@echo $(notdir $<)
	$(SILENTCMD)$(bin2o)

-include $(DEPENDS)

endif
