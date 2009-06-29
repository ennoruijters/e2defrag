SOURCES =  e2defrag.c io.c inode.c rbtree.c bmove.c bitmap.c debug.c
SOURCES += interactive.c freespace.c metadata_write.c metadata_read.c
SOURCES += algorithm.c crc16.c
OBJECTS =  e2defrag.o io.o inode.o rbtree.o bmove.o bitmap.o debug.o
OBJECTS += interactive.o freespace.o metadata_write.o metadata_read.o
OBJECTS += algorithm.o crc16.o
HEADERS = e2defrag.h rbtree.h extree.h crc16.h Makefile
CFLAGS += -ggdb -Wall -pedantic -std=gnu99

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
