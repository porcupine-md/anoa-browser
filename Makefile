# ── Detect platform & Qt prefix ─────────────────────────────────────────────
UNAME := $(shell uname -s)
ARCH  := $(shell uname -m)

ifeq ($(UNAME),Darwin)
  # Apple Silicon uses /opt/homebrew; Intel uses /usr/local
  ifeq ($(ARCH),arm64)
    QT_PREFIX ?= /opt/homebrew
  else
    QT_PREFIX ?= /usr/local
  endif
else
  QT_PREFIX ?= /usr
endif

# ── Directories ──────────────────────────────────────────────────────────────
BUILD_DIR        ?= build
BUILD_DIR_STATIC ?= build-static
BUILD_DIR_REL    ?= build-release
BUILD_DIR_RELST  ?= build-release-static
BUILD_DIR_COV    ?= build-coverage
INSTALL_PREFIX   ?= $(PWD)/dist

# Line-coverage floor enforced by `make coverage`, per file.
COVERAGE_MIN     ?= 80

# ── CMake flags ──────────────────────────────────────────────────────────────
CMAKE_BASE_FLAGS := \
  -DCMAKE_PREFIX_PATH=$(QT_PREFIX) \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

CMAKE_DEBUG   := $(CMAKE_BASE_FLAGS) -DCMAKE_BUILD_TYPE=Debug
CMAKE_RELEASE := $(CMAKE_BASE_FLAGS) -DCMAKE_BUILD_TYPE=Release

JOBS ?= $(shell nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)

# ── Website ──────────────────────────────────────────────────────────────────
# pages/ is a Cloudflare Pages project with no Git integration: merging to
# master publishes nothing, and the site has twice drifted releases behind.
PAGES_DIR     ?= pages
PAGES_PROJECT ?= anoa-browser
PAGES_URL     ?= https://anoa.jonggrang.dev
WRANGLER      ?= npx --yes wrangler@4

# Not a credential — an account id. The login carries eight accounts, and
# without this wrangler picks none and some subcommands return nothing
# instead of failing. Override it to keep it out of the repo.
CLOUDFLARE_ACCOUNT_ID ?= 9be807bd54837c5a15b24b573061f5bc

.PHONY: all build configure static release release-static \
        clean clean-static clean-release clean-release-static clean-all \
        install install-static \
        test coverage lint help \
        publish site-status

# ── Default ──────────────────────────────────────────────────────────────────
all: build

# ── Debug (dynamic) ──────────────────────────────────────────────────────────
configure:
	cmake -B $(BUILD_DIR) -S . $(CMAKE_DEBUG)

build: configure
	cmake --build $(BUILD_DIR) -- -j$(JOBS)

# ── Debug (static) ───────────────────────────────────────────────────────────
configure-static:
	cmake -B $(BUILD_DIR_STATIC) -S . $(CMAKE_DEBUG) -DSTATIC_BUILD=ON

static: configure-static
	cmake --build $(BUILD_DIR_STATIC) -- -j$(JOBS)

# ── Release (dynamic) ────────────────────────────────────────────────────────
configure-release:
	cmake -B $(BUILD_DIR_REL) -S . $(CMAKE_RELEASE)

release: configure-release
	cmake --build $(BUILD_DIR_REL) -- -j$(JOBS)

# ── Release (static) ─────────────────────────────────────────────────────────
configure-release-static:
	cmake -B $(BUILD_DIR_RELST) -S . $(CMAKE_RELEASE) -DSTATIC_BUILD=ON

release-static: configure-release-static
	cmake --build $(BUILD_DIR_RELST) -- -j$(JOBS)

# ── Install ───────────────────────────────────────────────────────────────────
install: release
	cmake --install $(BUILD_DIR_REL) --prefix $(INSTALL_PREFIX)

install-static: release-static
	cmake --install $(BUILD_DIR_RELST) --prefix $(INSTALL_PREFIX)

# ── Test ─────────────────────────────────────────────────────────────────────
test: configure
	cmake -B $(BUILD_DIR) -S . $(CMAKE_DEBUG) -DBUILD_TESTS=ON
	cmake --build $(BUILD_DIR) -- -j$(JOBS)
	cd $(BUILD_DIR) && ctest --output-on-failure

# ── Coverage ─────────────────────────────────────────────────────────────────
# gcov over the three Qt6::Core-only unit libraries. Its own build dir, because
# the instrumented objects are not the ones `make test` should be running.
coverage:
	BUILD_DIR=$(PWD)/$(BUILD_DIR_COV) QT_PREFIX=$(QT_PREFIX) \
	  COVERAGE_MIN=$(COVERAGE_MIN) bash tests/coverage.sh

# ── Lint / compile-commands symlink ──────────────────────────────────────────
lint: configure
	@if command -v clang-tidy >/dev/null 2>&1; then \
	  find src -name '*.cpp' | xargs clang-tidy -p $(BUILD_DIR); \
	else \
	  echo "clang-tidy not found — skipping"; \
	fi

# ── Clean ────────────────────────────────────────────────────────────────────
clean:
	rm -rf $(BUILD_DIR)

clean-static:
	rm -rf $(BUILD_DIR_STATIC)

clean-release:
	rm -rf $(BUILD_DIR_REL)

clean-release-static:
	rm -rf $(BUILD_DIR_RELST)

clean-all:
	rm -rf $(BUILD_DIR) $(BUILD_DIR_STATIC) $(BUILD_DIR_REL) $(BUILD_DIR_RELST) \
	       $(BUILD_DIR_COV) dist

# ── Website ──────────────────────────────────────────────────────────────────
# Deploys the working tree, not a commit. --branch is the branch you are on:
# Cloudflare treats only master as Production, so this from a feature branch
# gives a preview URL and leaves the live site alone.
publish:
	CLOUDFLARE_ACCOUNT_ID=$(CLOUDFLARE_ACCOUNT_ID) $(WRANGLER) pages deploy $(PAGES_DIR) \
	  --project-name $(PAGES_PROJECT) \
	  --branch "$$(git rev-parse --abbrev-ref HEAD)" \
	  --commit-hash "$$(git rev-parse HEAD)" \
	  --commit-message "$$(git log -1 --format=%s)"
	@$(MAKE) --no-print-directory site-status

# The question this project keeps getting wrong: merged has never meant
# published here. `publish` ends with it, because a deploy that published
# nothing looks exactly like one that worked.
site-status:
	@want=$$(sed -n 's/^project(anoa VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt); \
	 live=$$(curl -fsS $(PAGES_URL)/ | sed -n 's/.*latest <b>v\([0-9.]*\)<\/b>.*/\1/p' | head -1); \
	 echo "repo: $$want   live: $${live:-unknown}   $(PAGES_URL)"; \
	 [ "$$want" = "$$live" ] || echo "   ^ not the same — the deploy has not landed (or has not propagated yet)"

# ── Help ─────────────────────────────────────────────────────────────────────
help:
	@echo ""
	@echo "anoa build targets"
	@echo ""
	@echo "  make                        Debug build (dynamic)"
	@echo "  make build                  Debug build (dynamic)"
	@echo "  make static                 Debug build (static linking)"
	@echo "  make release                Release build (dynamic)"
	@echo "  make release-static         Release build (static linking)"
	@echo ""
	@echo "  make install                Install release build to INSTALL_PREFIX"
	@echo "  make install-static         Install static release build to INSTALL_PREFIX"
	@echo ""
	@echo "  make test                   Build and run tests"
	@echo "  make coverage               gcov the unit libraries, fail under COVERAGE_MIN%"
	@echo "  make lint                   Run clang-tidy (requires compile_commands.json)"
	@echo ""
	@echo "  make clean                  Remove debug build dir"
	@echo "  make clean-static           Remove static debug build dir"
	@echo "  make clean-release          Remove release build dir"
	@echo "  make clean-release-static   Remove static release build dir"
	@echo "  make clean-all              Remove all build dirs and dist/"
	@echo ""
	@echo "  make publish                Deploy $(PAGES_DIR)/ to Cloudflare Pages"
	@echo "  make site-status            Version in CMakeLists.txt vs the live site"
	@echo ""
	@echo "Variables (override on command line):"
	@echo "  QT_PREFIX=$(QT_PREFIX)"
	@echo "  INSTALL_PREFIX=$(INSTALL_PREFIX)"
	@echo "  JOBS=$(JOBS)"
	@echo "  PAGES_PROJECT=$(PAGES_PROJECT)"
	@echo ""
