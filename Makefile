# Copyright 2009 Enno Ruijters
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of version 2 of the GNU General
# Public License as published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

SOURCES =  e2defrag.c io.c inode.c rbtree.c bmove.c bitmap.c debug.c
SOURCES += interactive.c freespace.c metadata_write.c metadata_read.c
SOURCES += algorithm.c crc16.c
OBJECTS =  e2defrag.o io.o inode.o rbtree.o bmove.o bitmap.o debug.o
OBJECTS += interactive.o freespace.o metadata_write.o metadata_read.o
OBJECTS += algorithm.o crc16.o
HEADERS = e2defrag.h rbtree.h extree.h crc16.h Makefile
CFLAGS += -ggdb -Wall -pedantic -std=gnu99 -DNOSPLICE

ifndef ECHO
ECHO = @echo
endif

.PHONY: clean all

all: e2defrag

e2defrag: $(OBJECTS)
	$(ECHO) "	LN	e2defrag"
	@$(LINK.o) $(OBJECTS) $(LOADLIBES) $(LDLIBS) -o e2defrag

%.o: %.c $(HEADERS)
	$(ECHO) -e \\tCC\\t$@
	@$(CC) $(CFLAGS) $(TARGET_ARCH) -c $(OUTPUT_OPTION) $<

clean:
	$(ECHO) "	RM	e2defrag"
	-@$(RM) e2defrag
	$(ECHO) "	RM	*.o"
	-@$(RM) $(OBJECTS)
