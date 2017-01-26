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

include ../Makefile.def

LIBUNFS = libunfs.a
LIBUNFS_SO = libunfs.so

INCS := $(wildcard *.h)
OBJS := $(patsubst %.c,%.o,$(wildcard *.c))


all: $(LIBUNFS) $(LIBUNFS_SO)

$(LIBUNFS): unfs.o unfs_unvme.o unfs_raw.o
	$(AR) crs $@ $^

$(LIBUNFS_SO): $(LIBUNFS)
ifeq (,$(findstring UNFS_UNVME,$(CPPFLAGS)))
	$(CC) -shared -rdynamic -Wl,--whole-archive $^ -Wl,--no-whole-archive -o $@
else
	$(CC) -shared -rdynamic -Wl,--whole-archive $^ -lunvme -Wl,--no-whole-archive -o $@
endif

$(OBJS): $(INCS)

%.i: %.c
	$(CPP) $(CPPFLAGS) $< -o $@

lint: COPT = -D_FORTIFY_SOURCE=2 -O2
lint: clean $(OBJS)
	@$(RM) *.o

clean:
	$(RM) *.o *.i *.a *.so*

.PHONY: all raw unvme lint clean

