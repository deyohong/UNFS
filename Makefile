#
# Copyright (c) 2016-2017, Micron Technology, Inc.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
#   1. Redistributions of source code must retain the above copyright
#      notice, this list of conditions and the following disclaimer.
#
#   2. Redistributions in binary form must reproduce the above copyright
#      notice, this list of conditions and the following disclaimer in the
#      documentation and/or other materials provided with the distribution.
#
#   3. Neither the name of the copyright holder nor the names of its
#      contributors may be used to endorse or promote products derived
#      from this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#

include Makefile.def

INSTALLDIR ?= /usr/local

SUBDIRS := src test doc
ifneq (,$(WIREDTIGERDIR))
SUBDIRS += wiredtiger
endif

all: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -C $@

install: uninstall all
	mkdir -p $(INSTALLDIR)/include $(INSTALLDIR)/lib $(INSTALLDIR)/bin 
	/usr/bin/install -m644 src/unfs.h $(INSTALLDIR)/include
	/usr/bin/install -m755 test/unfs_{format,check,shell} $(INSTALLDIR)/bin
	/usr/bin/install -m644 src/libunfs*.a $(INSTALLDIR)/lib
	/usr/bin/install -m755 src/libunfs*.so $(INSTALLDIR)/lib
ifneq (,$(WIREDTIGERDIR))
	/usr/bin/install -m755 wiredtiger/libunfswt.so $(INSTALLDIR)/lib
endif

uninstall:
	$(RM) $(INSTALLDIR)/include/unfs.h	\
	      $(INSTALLDIR)/bin/unfs_*		\
	      $(INSTALLDIR)/lib/libunfs*

lint:
	@(for d in $(SUBDIRS); do $(MAKE) -C $$d lint; done)

clean:
	@(for d in $(SUBDIRS); do $(MAKE) -C $$d clean; done)

.PHONY: all install uninstall lint clean $(SUBDIRS)

