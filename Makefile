EXTENSION := timescaledb
BUILD_DIR ?= .build
VERSION := $(shell sed -n 's/^version[[:space:]]*=[[:space:]]*//p' $(CURDIR)/version.config)

# Find pg_config in PATH (set by build system)
PG_CONFIG := $(shell command -v pg_config 2>/dev/null)
PGROOT := $(shell $(PG_CONFIG) --bindir 2>/dev/null | sed 's,/bin$$,,')

EMCMAKE ?= emcmake
EMMAKE ?= emmake
CMAKE ?= cmake
EMCC ?= emcc


# Baseline configuration mirrors the pre-pglite TimescaleDB build:
# - Release build
# - Apache-only code path
# - Tests/telemetry disabled
# - OpenSSL off
# - Warnings not treated as errors
CMAKE_OPTS ?= \
	-DCMAKE_BUILD_TYPE=Release \
	-DAPACHE_ONLY=OFF \
	-DREGRESS_CHECKS=OFF \
	-DTAP_CHECKS=OFF \
	-DUSE_TELEMETRY=OFF \
	-DSEND_TELEMETRY_DEFAULT=OFF \
	-DUSE_OPENSSL=OFF \
	-DWARNINGS_AS_ERRORS=OFF \
	-DCMAKE_INSTALL_PREFIX=$(PGROOT) \
	-DPG_CONFIG=$(PG_CONFIG) \
	-DCMAKE_MODULE_LINKER_FLAGS="-sSIDE_MODULE=1" \
	-DCMAKE_SHARED_LINKER_FLAGS="-sSIDE_MODULE=1" \
	-DCMAKE_C_FLAGS="-fpic -sSIDE_MODULE=1" \
	-DBUILD_SHARED_LIBS=ON \
	-DCMAKE_POSITION_INDEPENDENT_CODE=ON

EXTRA_CMAKE_OPTS ?=

CMAKE_CACHE := $(BUILD_DIR)/CMakeCache.txt

.PHONY: all configure build install clean distclean

all: build

configure: $(CMAKE_CACHE)

$(CMAKE_CACHE):
	echo "--------------------------------"
	echo "PG_CONFIG: $(PG_CONFIG)"
	echo "PGROOT: $(PGROOT)"
	echo "pg_config (command -v): $(shell command -v $(PG_CONFIG) 2>/dev/null || echo not found)"
	mkdir -p $(BUILD_DIR)
	$(EMCMAKE) $(CMAKE) -S $(CURDIR) -B $(BUILD_DIR) $(CMAKE_OPTS) $(EXTRA_CMAKE_OPTS)

build: $(CMAKE_CACHE)
	$(EMMAKE) $(CMAKE) --build $(BUILD_DIR)

LLVM_NM ?= llvm-nm
# Match PGXS emscripten extension imports directory
EMSCRIPTEN_EXTENSION_IMPORTS_DIR := $(PGROOT)/include/postgresql/emscripten/extension/imports

install: build
	@echo "=== TimescaleDB install ==="
	DESTDIR=$(DESTDIR) $(EMMAKE) $(CMAKE) --install $(BUILD_DIR)
	@# Generate .imports file (mirrors pgxs.mk emscripten logic for MODULE_big)
	@# Extracts undefined symbols that TimescaleDB needs from pgcore
	@mkdir -p '$(EMSCRIPTEN_EXTENSION_IMPORTS_DIR)'
	@$(LLVM_NM) -u $$(find $(BUILD_DIR) -name '*.o') 2>/dev/null | awk '{print $$2}' | sed '/^$$/d' | sort -u > $(BUILD_DIR)/undef.txt
	@$(LLVM_NM) $$(find $(BUILD_DIR) -name '*.o') 2>/dev/null | awk '$$2 ~ /^[TDB]$$/ {print $$3}' | sed '/^$$/d' | sort -u > $(BUILD_DIR)/defs.txt
	@comm -23 $(BUILD_DIR)/undef.txt $(BUILD_DIR)/defs.txt > '$(EMSCRIPTEN_EXTENSION_IMPORTS_DIR)/timescaledb.imports'
	@rm -f $(BUILD_DIR)/undef.txt $(BUILD_DIR)/defs.txt
	@echo "Created $(EMSCRIPTEN_EXTENSION_IMPORTS_DIR)/timescaledb.imports ($$(wc -l < '$(EMSCRIPTEN_EXTENSION_IMPORTS_DIR)/timescaledb.imports') symbols)"

clean:
	@if [ -d "$(BUILD_DIR)" ]; then \
		$(EMMAKE) $(CMAKE) --build $(BUILD_DIR) --target clean || true; \
	fi

distclean:
	rm -rf "$(BUILD_DIR)"
