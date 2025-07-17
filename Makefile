#
# $Header$
#
# nsoracle --
#
#      Makefile for nsoracle database driver.
#

ifdef INST
   NSHOME ?= $(INST)
else ifdef NAVISERVER
   NSHOME ?= $(NAVISERVER)
else
    NSHOME = /usr/local/ns
endif

ifndef NAVISERVER
   NAVISERVER ?= $(NSHOME)
endif

#
# Version number used in release tags. Valid VERs are "1.1c", "2.1", 
# "2.2beta7". VER "1.1c" will be translated into "v1_1c" by this Makefile.
# Usage: make file-release VER=1.1c
#
VER_ = $(subst .,_,$(VER))

#
# Module Pretty-name (used to generalize the tag and file-release targets)
#
MODNAME  =  nsoracle

#
# Module name
#
MOD      =  nsoracle.so
MODCASS  =  nsoraclecass.so

#
# Objects to build
#
MODOBJS  =  nsoracle.o
OBJSCASS =  nsoraclecass.o

#
# Header files in THIS directory
#
HDRS     =  nsoracle.h

#
# Extra libraries
#
OCI_VERSION=$(shell strings $(ORACLE_HOME)/lib/libclntsh.so | grep "^Version.[0-9]\+\.[0-9]")
# So e.g. "Version 10.2.0.1.0" becomes just "10":
OCI_MAJOR_VERSION=$(shell echo $(OCI_VERSION) | cut -d ' ' -f2 | cut -d '.' -f1)
# With AOLserver this was in include/ns.h Naviserver moved it to here:
NS_VERSION=$(shell grep NS_VERSION $(NSHOME)/include/nsversion.h)

MODLIBS  +=  -L$(ORACLE_HOME)/lib -lclntsh -locci -lnsthread -lnsd -lnsdb

# TODO: Do we still need these additional libraries?  They do still
# exist at least for 10g, e.g. "libcore10.a":  --atp@piskorski.com, 2014/08/31
#   -lcore$(OCI_MAJOR_VERSION) -lcommon$(OCI_MAJOR_VERSION) -lgeneric$(OCI_MAJOR_VERSION) -lclient$(OCI_MAJOR_VERSION)

########################################################################

# Oracle 10.2.0 client include files are in "$ORACLE_HOME/rdbms/public",
# while 11.2 apparently puts them in "$ORACLE_HOME/sdk/include", so
# for simplicity just look in both places:

# Tack on the oracle includes after Makefile.global stomps CFLAGS
CFLAGS += -I$(ORACLE_HOME)/sdk/include \
    -I$(ORACLE_HOME)/rdbms/demo -I$(ORACLE_HOME)/rdbms/public \
    -I$(ORACLE_HOME)/network/public -I$(ORACLE_HOME)/plsql/public

ifdef ORA_CFLAGS
   CFLAGS += $(ORA_CFLAGS)
endif

include $(NSHOME)/include/Makefile.module

LD_LIBRARY_PATH	= LD_LIBRARY_PATH="./:$$LD_LIBRARY_PATH"

all: $(MOD) $(MODCASS) 

$(MODCASS): $(OBJSCASS)
	$(RM) $@
	$(LDSO) $(LDFLAGS) -o $@ $(OBJSCASS) $(MODLIBS) $(NSLIBS)

$(OBJSCASS): nsoracle.c $(HDRS)
	$(CC) $(CFLAGS) -DFOR_CASSANDRACLE=1 -o $@ -c $<

install: all
	$(RM) $(INSTBIN)/$(MOD)
	$(INSTALL_SH) $(MOD) $(INSTBIN)/
	$(RM) $(INSTBIN)/$(MODCASS)
	$(INSTALL_SH) $(MODCASS) $(INSTBIN)/

clean:
	$(RM) $(MODOBJS) $(MOD) $(OBJSCASS) $(MODCASS)

clobber: clean
	$(RM) *.so *.o *.a *~

distclean: clobber
	$(RM) TAGS core

#
# Help the poor developer
#
help:
	@echo "**" 
	@echo "** DEVELOPER HELP FOR THIS $(MODNAME)"
	@echo "**"
	@echo "** make tag VER=X.Y"
	@echo "**     Tags the module CVS code with the given tag."
	@echo "**     You can tag the CVS copy at any time, but follow the rules."
	@echo "**     VER must be of the form:"
	@echo "**         X.Y"
	@echo "**         X.YbetaN"
	@echo "**     You should browse CVS at SF to find the latest tag."
	@echo "**"
	@echo "** make file-release VER=X.Y"
	@echo "**     Checks out the code for the given tag from CVS."
	@echo "**     The result will be a releaseable tar.gz file of"
	@echo "**     the form: module-X.Y.tar.gz."
	@echo "**"

#
# Tag the code in CVS right now
#
tag:
	@if [ "$$VER" = "" ]; then echo 1>&2 "VER must be set to version number!"; exit 1; fi
	cvs rtag v$(VER_) $(MODNAME)

#
# Create a distribution file release
#
file-release:
	@if [ "$$VER" = "" ]; then echo 1>&2 "VER must be set to version number!"; exit 1; fi
	rm -rf work
	mkdir work
	cd work && cvs -d :pserver:anonymous@cvs.aolserver.sourceforge.net:/cvsroot/aolserver co -r v$(VER_) $(MODNAME)
	mv work/$(MODNAME) work/$(MODNAME)-$(VER)
	( cd work && tar cvf - $(MODNAME)-$(VER) ) | gzip -9 > $(MODNAME)-$(VER).tar.gz
	rm -rf work

