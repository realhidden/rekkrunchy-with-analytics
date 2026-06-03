# rekkrunchy — cross-platform context-mixing compressor.
#
#   make            build the portable `rekkrunchy` CLI (native gcc, any arch)
#   make test       quick self-roundtrip of the CLI
#   make docker-test   build + run the full corpus test (incl. x86 decoder) in
#                      the 32-bit docker image (needed for the x86 self-decoder)
#
CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
SRC      = src/main.c src/codec.c src/model.c src/tables.c src/x86filter.c

all: rekkrunchy

# src/tables.c and x86/depack.asm are committed, generated artifacts; regenerate
# them only when the originals change, via `make gen-tables` / `make gen-depack`.
rekkrunchy: $(SRC) src/codec.h src/model.h src/x86filter.h
	$(CC) $(CFLAGS) -Isrc -o $@ $(SRC)

# Quick sanity roundtrip using the tool on its own source.
test: rekkrunchy
	./rekkrunchy -c src/model.c /tmp/rk.c
	./rekkrunchy -d /tmp/rk.c /tmp/rk.d
	cmp src/model.c /tmp/rk.d && echo "roundtrip OK"

# --- docker (32-bit x86 toolchain for the asm decoder + corpus test) ----------

docker-build:
	docker build --platform=linux/386 -t rekk-build .

docker-test: docker-build
	docker run --rm --platform=linux/386 -v "$(CURDIR)":/app rekk-build \
	  sh -c 'cd /app && sh test/corpus_test.sh'

# Report the .text byte size of each standalone x86 decoder.
decoder-size:
	docker run --rm --platform=linux/386 -v "$(CURDIR)":/app rekk-build sh -c \
	  'cd /app; for a in x86/depack.asm x86/unfilter.asm; do \
	     [ -f "$$a" ] || continue; nasm -f elf32 -o /tmp/s.o "$$a" 2>/dev/null && \
	     printf "%-22s %5d bytes .text\n" "$$a" "$$(size -A /tmp/s.o | awk "/\\.text/{print \$$2}")"; \
	   done'

# --- regenerate baked artifacts (needs the i386 docker image) -----------------

gen-depack:
	python3 x86/gen_depack.py

gen-tables:
	python3 test/gen_oracle.py model_asm.asm test/oracle.asm
	docker run --rm --platform=linux/386 -v "$(CURDIR)":/app rekk-build sh -c \
	  'cd /app && nasm -f elf32 -o /tmp/o.o test/oracle.asm && \
	   gcc -m32 -O2 -w -o /tmp/d test/dump_tables.c /tmp/o.o && \
	   /tmp/d > /tmp/tables.c && cp /tmp/tables.c src/tables.c'

clean:
	rm -f rekkrunchy

.PHONY: all test docker-build docker-test gen-depack gen-tables clean
