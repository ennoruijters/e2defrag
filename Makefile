SOURCES = e2defrag.c io.c inode.c rbtree.c bmove.c bitmap.c debug.c interactive.c freespace.c
OBJECTS = e2defrag.o io.o inode.o rbtree.o bmove.o bitmap.o debug.o interactive.o freespace.o
HEADERS = e2defrag.h rbtree.h extree.h Makefile
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
