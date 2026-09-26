# mosm: build, test, benchmark.
#
#   make build   build the mosm executable
#   make test    build, run every command on test/reads.fa, diff against test/exp.txt
#   make bench   run the benchmark suite (bench/run.sh) on the gencode FASTA
#   make analyze run the factor decomposition (bench/analyze.sh)
#   make clean   remove build products and test/bench output
#
# Every build and run is under a memory cap and a timeout: the compiler and
# the pools it generates are under development, and either can allocate
# without bound on a novel input.

EXE := mosm
SRC := main.loc
FIX := test/reads.fa
OBS := test/obs.txt
EXP := test/exp.txt
FASTA ?= data/gencode.v50.transcripts.fa

# A guarded command is one word, so it composes in a pipeline.
GUARD := sh -c 'ulimit -v 8000000 && exec timeout 900 "$$@"' guard
RUN := $(GUARD) ./$(EXE) --quiet

.PHONY: build test bench analyze clean

build:
	$(GUARD) env GHCRTS=-M2g morloc make -o $(EXE) $(SRC)

# Every subcommand once through a file and once through stdin, plus every
# error path, so a change in the Try handling shows up as a diff. The stream
# file is rebuilt from the fixture each run; --quiet keeps the BENCH rows out.
test: build
	: > $(OBS)
	$(RUN) -f packet -z 3 convert $(FIX) > test/reads.dat
	echo '# cut 3'                       >> $(OBS); $(RUN) cut 3 test/reads.dat            >> $(OBS)
	echo '# cut 1:4:2 (stdin)'           >> $(OBS); $(RUN) cut 1:4:2 < test/reads.dat      >> $(OBS)
	echo '# cut 10:'                     >> $(OBS); $(RUN) cut 10: test/reads.dat          >> $(OBS)
	echo '# filter -l 1000 -s 2000'      >> $(OBS); $(RUN) filter -l 1000 -s 2000 test/reads.dat | grep '>' >> $(OBS)
	echo '# filter -c "GC > .45" -t 1'   >> $(OBS); $(RUN) filter -c 'GC > .45' -t 1 test/reads.dat | grep '>' >> $(OBS)
	echo '# filter -c "GC > .45" -t 0 (stdin)' >> $(OBS); $(RUN) filter -c 'GC > .45' < test/reads.dat | grep '>' >> $(OBS)
	echo '# stat'                        >> $(OBS); $(RUN) stat test/reads.dat             >> $(OBS)
	echo '# stat -g (stdin)'             >> $(OBS); $(RUN) stat -g < test/reads.dat        >> $(OBS)
	echo '# cstat -p'                    >> $(OBS); $(RUN) cstat -p test/reads.dat         >> $(OBS)
	echo '# cstat -I -c'                 >> $(OBS); $(RUN) cstat -I -c test/reads.dat      >> $(OBS)
	echo '# cstat -s -t 1'               >> $(OBS); $(RUN) cstat -s -t 1 test/reads.dat    >> $(OBS)
	echo '# convert (stdin) | cut 0'     >> $(OBS); $(RUN) -f packet convert < $(FIX) | $(RUN) cut 0 >> $(OBS)
	echo '# cut abc (bad index)'         >> $(OBS); $(RUN) cut abc test/reads.dat 2>&1 | head -2   >> $(OBS); true
	echo '# cut 1:x (bad slice)'         >> $(OBS); $(RUN) cut 1:x test/reads.dat 2>&1 | head -2   >> $(OBS); true
	echo '# cut 1:2:3:4 (too many parts)' >> $(OBS); $(RUN) cut 1:2:3:4 test/reads.dat 2>&1 | head -2 >> $(OBS); true
	echo '# filter -c "GC > 2"'          >> $(OBS); $(RUN) filter -c 'GC > 2' test/reads.dat 2>&1 | head -2 >> $(OBS); true
	echo '# filter -c "GC = .5"'         >> $(OBS); $(RUN) filter -c 'GC = .5' test/reads.dat 2>&1 | head -2 >> $(OBS); true
	echo '# filter -c "GC"'              >> $(OBS); $(RUN) filter -c 'GC' test/reads.dat 2>&1 | head -2 >> $(OBS); true
	echo '# stat on a raw FASTA'         >> $(OBS); $(RUN) stat $(FIX) 2>&1 | head -1 >> $(OBS); true
	diff -u $(EXP) $(OBS)

bench: build
	bench/run.sh $(FASTA)

analyze: build
	bench/analyze.sh $(FASTA)

clean:
	rm -rf $(EXE) $(EXE)-build $(OBS) test/reads.dat
	rm -f bench/*.dat bench/last.err bench/run.log bench/analyze.log bench/out.fa
