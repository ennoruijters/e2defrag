SOURCES = e2defrag.c io.c inode.c rbtree.c bmove.c bitmap.c debug.c interactive.c
OBJECTS = e2defrag.o io.o inode.o rbtree.o bmove.o bitmap.o debug.o interactive.o
HEADERS = e2defrag.h rbtree.h extree.h Makefile
CFLAGS += -ggdb -Wall -pedantic -std=gnu99

.PHONY: clean

e2defrag: $(OBJECTS)
	$(CC) $(CFLAGS) -o e2defrag $(OBJECTS)

$(OBJECTS): $(HEADERS)

clean:
	$(RM) e2defrag $(OBJECTS)
