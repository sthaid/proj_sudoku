
TARGETS = sudoku

CC = gcc
CFLAGS = -g -O2 -pthread -Wall -lrt

all: $(TARGETS)

#
# build rules
#

sudoku: sudoku.c
	$(CC) $(CFLAGS) $< -o $@

#
# clean rule
#

clean:
	rm -f $(TARGETS)

