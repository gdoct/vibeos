# VibeOS TCP receive-path fuzzer.
# Run from the repo root as:  make -C fuzz <target>
# (CWD becomes fuzz/, so ".." is the repo root.)

CXX     := clang++   # override on the CLI: make CXX=clang++-18
STD      := -std=c++17
INC      := -I stub -I ../kernel/src
SAN      := -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined
SRC      := tcpfuzz.cc stub/kernel.h stub/task.h stub/timer.h stub/csprng.h stub/net.h

# Tunables for `make run`
TIME    ?= 60          # max_total_time (seconds)
LEN     ?= 1600        # max input length
JOBS    ?= 1

.PHONY: all run dbg cov clean
all: tcpfuzz

# Coverage-guided fuzzer (ASan + UBSan + libFuzzer).
tcpfuzz: $(SRC)
	$(CXX) $(STD) -g -O1 $(SAN) $(INC) tcpfuzz.cc -o $@

# Run a campaign. Crashes/timeouts land in artifacts/.
run: tcpfuzz
	@mkdir -p corpus artifacts
	./tcpfuzz -timeout=10 -max_total_time=$(TIME) -max_len=$(LEN) \
	          -dict=tcp.dict -artifact_prefix=artifacts/ corpus

# Standalone replay build with a SIGALRM backtrace dumper, for triaging a hang:
#   make -C fuzz dbg && ./fuzz/tcpdbg fuzz/artifacts/timeout-XXXX
dbg: $(SRC)
	$(CXX) $(STD) -g -O0 -rdynamic -DSTANDALONE $(INC) tcpfuzz.cc -o tcpdbg

# Per-function coverage of the receive path over the current corpus.
cov: $(SRC)
	@mkdir -p corpus
	$(CXX) $(STD) -O1 -DSTANDALONE -fprofile-instr-generate -fcoverage-mapping \
	       $(INC) tcpfuzz.cc -o tcpcov
	@rm -f cov_*.profraw; i=0; for f in corpus/*; do \
	   LLVM_PROFILE_FILE="cov_$$i.profraw" ./tcpcov "$$f" >/dev/null 2>&1; i=$$((i+1)); done
	@llvm-profdata merge -sparse cov_*.profraw -o tcp.profdata
	@llvm-cov report tcpcov -instr-profile=tcp.profdata -show-functions ../kernel/src/net.c

clean:
	rm -f tcpfuzz tcpdbg tcpcov *.profraw tcp.profdata
