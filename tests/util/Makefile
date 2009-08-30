CFLAGS += -ggdb -Wall -pedantic -std=gnu99

ifndef ECHO
ECHO = @echo
endif

.PHONY: clean all

all: falloc

falloc: falloc.o
	$(ECHO) "	LN	falloc"
	@$(LINK.o) falloc.o $(LOADLIBES) $(LDLIBS) -o falloc

%.o: %.c
	$(ECHO) -e \\tCC\\t$@
	@$(CC) $(CFLAGS) $(TARGET_ARCH) -c $(OUTPUT_OPTION) $<

clean:
	$(ECHO) "	RM	falloc"
	-@$(RM) falloc
	$(ECHO) "	RM	*.o"
	-@$(RM) *.o
