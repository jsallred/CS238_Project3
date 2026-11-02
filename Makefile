#
# Tony Givargis
# Copyright (C), 2023-2026
# University of California, Irvine
#
# CS 238P - Operating Systems
# Makefile
#

CC     = gcc
CFLAGS = -ansi -pedantic -Wall -Wextra -Werror -Wfatal-errors -fpic -O3
LDLIBS =
DEST   = cs238
SRCS  := $(wildcard *.c)
OBJS  := $(SRCS:.c=.o)

all: $(OBJS)
	@echo "[LN]" $(DEST)
	@$(CC) -o $(DEST) $(OBJS) $(LDLIBS)

%.o: %.c
	@echo "[CC]" $<
	@$(CC) $(CFLAGS) -c $<
	@$(CC) $(CFLAGS) -MM $< > $*.d

clean:
	@rm -f $(DEST) tests/test_extra tests/drive_shell.o \
		test_extra.scm test_shell.scm test_flags.scm test_color.scm \
		test_words.txt test_delete_me.txt *.so *.o *.d *~ *#

.PHONY: test

test: all tests/test_extra
	./tests/test_extra

tests/drive_shell.o: tests/drive_shell.c tests/drive_shell.h
	@echo "[CC]" tests/drive_shell.c
	@$(CC) -std=gnu99 -Wall -Wextra -Werror -Wfatal-errors -fpic -O2 -Itests -c tests/drive_shell.c -o tests/drive_shell.o

tests/test_extra: tests/test_extra.c tests/drive_shell.o scm.c avl.c system.c shell.c term.c scm.h avl.h system.h shell.h term.h tests/drive_shell.h
	@echo "[CC]" tests/test_extra.c
	@$(CC) $(CFLAGS) -I. -Itests -o tests/test_extra tests/test_extra.c tests/drive_shell.o scm.c avl.c system.c shell.c term.c $(LDLIBS)

-include $(OBJS:.o=.d)
