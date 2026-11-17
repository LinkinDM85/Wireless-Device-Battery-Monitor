# Builds WirelessBattery.exe with MinGW-w64 (MSYS2 MINGW64/UCRT64 shell, or any mingw g++ on PATH).
CXX      ?= g++
WINDRES  ?= windres
CXXFLAGS ?= -std=c++17 -Os -Wall -Wno-missing-field-initializers -ffunction-sections -fdata-sections \
            -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00
LDFLAGS  ?= -municode -mwindows -static -s -Wl,--gc-sections
LIBS     := -lhid -lsetupapi -lcfgmgr32 -lole32 -loleaut32 -lcomctl32 -lgdi32 -lruntimeobject -lws2_32

SRC := $(wildcard src/*.cpp) $(wildcard src/drivers/*.cpp)
OBJ := $(patsubst src/%.cpp,build/%.o,$(SRC)) build/app_res.o

WirelessBattery.exe: $(OBJ)
	$(CXX) $(LDFLAGS) $^ -o $@ $(LIBS)

build/%.o: src/%.cpp $(wildcard src/*.h src/drivers/*.h)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -municode -c $< -o $@

build/app_res.o: res/app.rc res/app.manifest res/app.ico
	@mkdir -p build
	$(WINDRES) -i $< -o $@ --include-dir res

clean:
	rm -rf build WirelessBattery.exe

# Every tests/*_test.cpp is built and run. The first line of each test names the sources it links
# against ("// test-sources: src/drivers/x.cpp" or "(none)"), so a new test needs no Makefile change.
# The Linux HID test needs the Linux backend and runs from build-linux.sh instead.
TESTS := $(filter-out tests/linux_hid_test.cpp,$(sort $(wildcard tests/*_test.cpp)))

test:
	@mkdir -p build/tests
	@failed=""; \
	for t in $(TESTS); do \
	    n=$$(basename $$t .cpp); \
	    src=$$(sed -n '1s|^// test-sources:||p' $$t); \
	    case "$$src" in *"(none)"*) src="";; esac; \
	    if [ -z "$$(sed -n '1p' $$t | grep '^// test-sources:')" ]; then \
	        echo "$$t: first line must be // test-sources: ..."; failed="$$failed $$n"; continue; \
	    fi; \
	    echo "== $$n"; \
	    if $(CXX) $(CXXFLAGS) $$t $$src -o build/tests/$$n.exe $(LIBS) && ./build/tests/$$n.exe; then :; \
	    else failed="$$failed $$n"; [ -n "$$GITHUB_ACTIONS" ] && echo "::error title=Test failed::$$n"; fi; \
	done; \
	if [ -n "$$failed" ]; then echo "FAILED:$$failed"; exit 1; fi; \
	echo "All $(words $(TESTS)) tests passed."

.PHONY: clean test
