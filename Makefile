#---------------------------------------------------------------------------------
# Clear the implicit built in rules
#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------
ifeq ($(strip $(PSL1GHT)),)
$(error "Please set PSL1GHT in your environment. export PSL1GHT=<path>")
endif

include $(PSL1GHT)/ppu_rules

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# INCLUDES is a list of directories containing extra header files
#---------------------------------------------------------------------------------
TARGET		:=	psxripper
BUILD		:=	build
SOURCES		:=	source fatfs
DATA		:=
INCLUDES	:=	include fatfs

ICON0		:=	$(CURDIR)/pkgfiles/ICON0.PNG
# ICON0.PNG and PIC1.PNG (the XMB background) go into the package
PKGFILES	:=	$(CURDIR)/pkgfiles
APPID		:=	PSXR00001
CONTENTID	:=	UP0001-$(APPID)_00-0000000000000000

# The version: version.txt (semantic versioning, bumped by the release workflow). PARAM.SFO takes
# "MM.mm", so APP_VER is the major and minor version; the app shows the full one.
VERSION		:=	$(shell cat $(dir $(firstword $(MAKEFILE_LIST)))version.txt 2>/dev/null || echo 0.0.0)
APP_VER		:=	$(shell echo $(VERSION) | awk -F. '{printf "%02d.%02d", $$1, $$2}')
# the name in the XMB, with the version as other homebrew shows it
TITLE		:=	PSX Ripper v$(VERSION)
#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------

CFLAGS		=	-O2 -Wall -mcpu=cell $(MACHDEP) $(INCLUDE)
ifeq ($(PRIVATE_SBI),1)
CFLAGS	+=	-DPRIVATE_SBI
endif
CXXFLAGS	=	$(CFLAGS)

LDFLAGS		=	$(MACHDEP) -Wl,-Map,$(notdir $@).map

#---------------------------------------------------------------------------------
# any extra libraries we wish to link with the project
#---------------------------------------------------------------------------------
LIBS	:=	-lz -lrsx -lgcm_sys -lio -lsysutil -lnetctl -lnet -lusb -lsysmodule -lrt -llv2 -lm

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:=	$(PS3DEV)/portlibs/ppu

#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
					$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

export BUILDDIR	:=	$(CURDIR)/$(BUILD)

#---------------------------------------------------------------------------------
# automatically build a list of object files for our project
#---------------------------------------------------------------------------------
CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
sFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.S)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
	export LD	:=	$(CC)
else
	export LD	:=	$(CXX)
endif

export OFILES	:=	$(addsuffix .o,$(BINFILES)) \
					$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) \
					$(sFILES:.s=.o) $(SFILES:.S=.o)

#---------------------------------------------------------------------------------
# build a list of include paths
#---------------------------------------------------------------------------------
export INCLUDE	:=	$(foreach dir,$(INCLUDES), -I$(CURDIR)/$(dir)) \
					$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
					$(LIBPSL1GHT_INC) \
					-I$(CURDIR)/$(BUILD)

#---------------------------------------------------------------------------------
# build a list of library paths
#---------------------------------------------------------------------------------
export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib) \
					$(LIBPSL1GHT_LIB)

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export SFOXML	:=	$(CURDIR)/$(BUILD)/sfo.xml
.PHONY: $(BUILD) clean pkg version

version:
	@echo $(VERSION)

#---------------------------------------------------------------------------------
pkg: $(BUILD) $(OUTPUT).pkg
# the package also changes with its name, version, metadata and the XMB pictures
$(OUTPUT).pkg: $(CURDIR)/Makefile $(CURDIR)/version.txt $(CURDIR)/tools/sfo.xml.in $(wildcard $(CURDIR)/pkgfiles/*)

#---------------------------------------------------------------------------------
$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@echo '#define APP_VERSION "$(VERSION)"' > $@/version.h.new
	@cmp -s $@/version.h.new $@/version.h && rm $@/version.h.new || mv $@/version.h.new $@/version.h
	@sed 's/@APP_VER@/$(APP_VER)/' tools/sfo.xml.in > $@/sfo.xml
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).self $(OUTPUT).pkg

#---------------------------------------------------------------------------------
run:
	ps3load $(OUTPUT).self


#---------------------------------------------------------------------------------
else

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
$(OUTPUT).self: $(OUTPUT).elf
$(OUTPUT).elf:	$(OFILES)

#---------------------------------------------------------------------------------
# This rule links in binary data with the .bin extension
#---------------------------------------------------------------------------------
%.bin.o	:	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
