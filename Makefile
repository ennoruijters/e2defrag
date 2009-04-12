SOURCES = e2defrag.c io.c inode.c rbtree.c
OBJECTS = e2defrag.o io.o inode.o rbtree.o
HEADERS = e2defrag.h rbtree.h extree.h
CFLAGS += -ggdb -Wall -pedantic -std=gnu99

.PHONY: clean

e2defrag: $(OBJECTS)
	$(CC) $(CFLAGS) -o e2defrag $(OBJECTS)

$(OBJECTS): $(HEADERS)

clean:
	$(RM) e2defrag $(OBJECTS)
