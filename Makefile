CXX      := g++
CXXFLAGS := -O3 -std=c++17 -Wall -Wextra -pthread
LIBFLAGS := -shared -fPIC -ldl

TARGET_LIB   := libananth_eflash.so
TEST_BIN     := tests/stress_test
BENCH_BIN    := benchmarks/bench_suite

.PHONY: all lib test bench clean

all: lib

lib: $(TARGET_LIB)

$(TARGET_LIB): malloc_hijack.cpp flash.h
	$(CXX) $(CXXFLAGS) $(LIBFLAGS) malloc_hijack.cpp -o $(TARGET_LIB)

$(TEST_BIN): tests/stress_test.cpp flash.h
	$(CXX) $(CXXFLAGS) -I. tests/stress_test.cpp -o $(TEST_BIN) -ldl

$(BENCH_BIN): benchmarks/bench_suite.cpp
	$(CXX) $(CXXFLAGS) benchmarks/bench_suite.cpp -o $(BENCH_BIN)

test: lib $(TEST_BIN)
	@echo "=== Running Correctness & Concurrency Stress Suite (LD_PRELOAD) ==="
	LD_PRELOAD=./$(TARGET_LIB) ./$(TEST_BIN)
	@echo "=== Verifying Real Linux Binary Hijack (LD_PRELOAD) ==="
	LD_PRELOAD=./$(TARGET_LIB) /bin/ls /tmp > /dev/null
	LD_PRELOAD=./$(TARGET_LIB) python3 -c "a=[bytearray(1024) for _ in range(500)]; print('Python3 LD_PRELOAD OK:', len(a))"

bench: lib $(BENCH_BIN)
	@echo "=== Baseline: System glibc malloc ==="
	./$(BENCH_BIN) glibc
	@echo ""
	@echo "=== Ananth E-Flash V2.0 (LD_PRELOAD) ==="
	LD_PRELOAD=./$(TARGET_LIB) ./$(BENCH_BIN) eflash

clean:
	rm -f $(TARGET_LIB) $(TEST_BIN) $(BENCH_BIN)
