# Nasdaq-faithful engine. `make help` lists targets.
BUILD ?= build
JOBS  ?= $(shell nproc)

.PHONY: help build spec test fuzz fuzz-libfuzzer mutants reproduce extract bench cpptrader clean

help:
	@echo "make build           C++ tools and tests (Release)"
	@echo "make spec            OCaml executable spec"
	@echo "make test            unit, property, equivalence, QCheck and 1M-op differential tests"
	@echo "make fuzz-libfuzzer  coverage-guided fuzzing of parser and engine (clang)"
	@echo "make mutants         seeded-bug matrix (needs synthetic data: run reproduce once)"
	@echo "make reproduce       every number and chart in results/ (synthetic by default)"
	@echo "                     real data: make reproduce ITCH='data/01302020.NASDAQ_ITCH50' SYMBOLS=AAPL,MSFT"
	@echo "make extract IN=... OUT=... SYMBOLS=A,B,C   cut a symbol extract"
	@echo "make bench FILE=...  throughput and latency, both book designs"
	@echo "make cpptrader FILE=...  external baseline on the same file"

build:
	cmake -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD) -j$(JOBS)

spec:
	cd spec && dune build

test: build spec
	cd $(BUILD) && ctest --output-on-failure
	cd spec && dune test
	python3 fuzz/difftest.py --regressions
	python3 fuzz/difftest.py --ops 1000000 --seed $${SEED:-1}

sanitize:
	cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DNFE_SANITIZE=ON
	cmake --build build-asan -j$(JOBS)
	cd build-asan && ctest --output-on-failure
	python3 fuzz/difftest.py --ops 200000 --cpp build-asan/nfe_engine

fuzz-libfuzzer:
	cmake -S . -B build-fuzz -DNFE_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++
	cmake --build build-fuzz -j$(JOBS) --target fuzz_parser fuzz_engine
	mkdir -p fuzz/corpus/parser fuzz/corpus/engine
	./build-fuzz/fuzz_parser -max_total_time=$${SECS:-60} fuzz/corpus/parser
	./build-fuzz/fuzz_engine -max_total_time=$${SECS:-60} fuzz/corpus/engine

mutants: build spec
	python3 mutants/make_patches.py
	python3 mutants/run_mutants.py --itch data/synth1.itch --truth data/truth1.jsonl \
	  --corrupt data/corrupt.itch --corrupt-expected data/corrupt_expected.json --jobs $(JOBS)

reproduce:
	ITCH="$(ITCH)" SYMBOLS="$(SYMBOLS)" scripts/reproduce.sh

extract: build
	$(BUILD)/nfe_extract $(IN) $(OUT) $(SYMBOLS)

bench: build
	scripts/bench.sh $(FILE) results/bench.json

cpptrader:
	scripts/cpptrader_baseline.sh $(FILE) results/cpptrader.json

clean:
	rm -rf build build-asan build-fuzz out spec/_build
