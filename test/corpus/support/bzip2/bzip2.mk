# The corpus gate's build of bzip2 (test/corpus/manifest.json, config bzip2): the pinned
# checkout has only CMake and Meson builds, so this mimics the classic upstream Makefile
# (libbz2.a, bzip2 linked against it, bzip2recover) and its `make test` of the six samples.
CFLAGS ?= -O2
OBJS = blocksort.o huffman.o crctable.o randtable.o compress.o decompress.o bzlib.o

all: bzip2 bzip2recover bzip2-direct

bz_version.h: bz_version.h.in
	sed 's/@BZ_VERSION@/1.1.0/' $< > $@

bzlib.o: bz_version.h

%.o: %.c
	$(CC) $(CFLAGS) -D_FILE_OFFSET_BITS=64 -DBZ_UNIX=1 -DBZ_LCCWIN32=0 -c $< -o $@

libbz2.a: $(OBJS)
	rm -f $@
	ar cq $@ $(OBJS)
	ranlib $@

bzip2: libbz2.a bzip2.o
	$(CC) $(CFLAGS) -o bzip2 bzip2.o -L. -lbz2

# Same program linked from objects, so the link step sees every record.
bzip2-direct: $(OBJS) bzip2.o
	$(CC) $(CFLAGS) -o $@ bzip2.o $(OBJS)

bzip2recover: bzip2recover.o
	$(CC) $(CFLAGS) -o bzip2recover bzip2recover.o

test: bzip2 bzip2-direct
	@for B in ./bzip2 ./bzip2-direct; do \
	 set -e; \
	 $$B -1  < tests/sample1.ref > sample1.rb2; \
	 $$B -2  < tests/sample2.ref > sample2.rb2; \
	 $$B -3  < tests/sample3.ref > sample3.rb2; \
	 $$B -d  < tests/sample1.bz2 > sample1.tst; \
	 $$B -d  < tests/sample2.bz2 > sample2.tst; \
	 $$B -ds < tests/sample3.bz2 > sample3.tst; \
	 cmp tests/sample1.bz2 sample1.rb2; \
	 cmp tests/sample2.bz2 sample2.rb2; \
	 cmp tests/sample3.bz2 sample3.rb2; \
	 cmp sample1.tst tests/sample1.ref; \
	 cmp sample2.tst tests/sample2.ref; \
	 cmp sample3.tst tests/sample3.ref; \
	 echo "$$B: 6 sample tests OK"; \
	done
