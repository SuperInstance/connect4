# connect4 -- exact Connect 4 ground truth
#
#   make            build ./ctool
#   make selftest   representation + layout + encoding checks, no solving
#   make count      distinct legal position census, no solving
#   make export     self-checks, known-answer checks, then the ply-bounded export
#   make clean

CC      ?= cc
CFLAGS  ?= -std=c11 -O3 -Wall -Wextra
LDFLAGS ?=

# The export is a single self-contained C11 program with no dependencies and
# no libraries beyond libc.  Integer-only arithmetic, so the output is
# bit-identical whatever the machine.
ctool: ctool.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

.PHONY: selftest count export clean
selftest: ctool
	./ctool selftest

count: ctool
	./ctool count

# PLYS is the requested ply bound.  The program reports the bound it actually
# achieved; it never quietly lowers it.  The defaults are tuned for a 1-core
# sandbox: the ply-2 stage alone is 49 full game solves.
PLYS      ?= 12
TOTAL_SEC ?= 10800
export: ctool
	@mkdir -p gt
	./ctool export --plys $(PLYS) --total-seconds $(TOTAL_SEC) --out gt

clean:
	rm -f ctool
