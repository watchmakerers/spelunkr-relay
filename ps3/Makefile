#---------------------------------------------------------------------------------
# SPELUNKR for PS3: channel guide + player. Talks to the relay in ../relay on your PC.
#   make        -> spelunkr.self      make pkg -> spelunkr.pkg (install with HEN/CFW)
#---------------------------------------------------------------------------------
.SUFFIXES:
ifeq ($(strip $(PSL1GHT)),)
$(error "Please set PSL1GHT in your environment. export PSL1GHT=<path>")
endif

include $(PSL1GHT)/ppu_rules

TARGET		:=	spelunkr
BUILD		:=	build
SOURCES		:=	source
DATA		:=	data
SHADERS		:=	shaders
INCLUDES	:=	include

TITLE		:=	SPELUNKR
APPID		:=	SPLK00001
CONTENTID	:=	UP0001-$(APPID)_00-0000000000000000
ICON0		:=	$(CURDIR)/pkgfiles/ICON0.PNG

CFLAGS		=	-O2 -Wall -mcpu=cell $(MACHDEP) $(INCLUDE)
CXXFLAGS	=	$(CFLAGS)
LDFLAGS		=	$(MACHDEP) -Wl,-Map,$(notdir $@).map

LIBS	:=	-lrsx -lgcm_sys -lio -lsysutil -laudio -lnet -lsysmodule -lrt -llv2 -lm
LIBDIRS	:=

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
					$(foreach dir,$(DATA),$(CURDIR)/$(dir)) \
					$(foreach dir,$(SHADERS),$(CURDIR)/$(dir))
export DEPSDIR	:=	$(CURDIR)/$(BUILD)
export BUILDDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.ttf)))
# Shaders are precompiled (shaders/src has the Cg sources and how to rebuild them): NVIDIA's
# Cg compiler that PSL1GHT's cgcomp needs only runs on glibc, not in the Alpine toolchain image.
VPOFILES	:=	$(foreach dir,$(SHADERS),$(notdir $(wildcard $(dir)/*.vpo)))
FPOFILES	:=	$(foreach dir,$(SHADERS),$(notdir $(wildcard $(dir)/*.fpo)))

export LD	:=	$(CC)
export OFILES	:=	$(addsuffix .o,$(BINFILES)) \
					$(addsuffix .o,$(VPOFILES)) \
					$(addsuffix .o,$(FPOFILES)) \
					$(CFILES:.c=.o)

export INCLUDE	:=	$(foreach dir,$(INCLUDES), -I$(CURDIR)/$(dir)) \
					$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
					$(LIBPSL1GHT_INC) \
					-I$(CURDIR)/$(BUILD)
export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib) \
					$(LIBPSL1GHT_LIB)

.PHONY: $(BUILD) clean pkg run

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).self $(OUTPUT)*.pkg

run:
	ps3load $(OUTPUT).self

pkg:	$(BUILD) $(OUTPUT).pkg

else

DEPENDS	:=	$(OFILES:.o=.d)

$(OUTPUT).self: $(OUTPUT).elf
$(OUTPUT).elf:	$(OFILES)

# Objects that include generated headers (shaders, font) must wait for them.
$(CFILES:.c=.o): $(addsuffix .o,$(BINFILES)) $(addsuffix .o,$(VPOFILES)) $(addsuffix .o,$(FPOFILES))

%.ttf.o	:	%.ttf
	@echo $(notdir $<)
	@$(bin2o)
%.vpo.o	:	%.vpo
	@echo $(notdir $<)
	@$(bin2o)
%.fpo.o	:	%.fpo
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

endif
