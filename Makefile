CC      := cc
PYTHON  ?= python3
UNAME_S := $(shell uname -s)
PKG_CONFIG ?= pkg-config
PKG_DEPS   := libxml-2.0 libcurl openssl jansson

GIT_HASH  := $(shell git rev-parse --short HEAD)
GIT_DATE  := $(shell git log -1 --format='%ad' --date='format:%y.%m.%d')
GIT_BRANCH := $(shell git rev-parse --abbrev-ref HEAD)
GIT_REMOTE := $(shell git remote get-url origin)
GIT_TAG   := $(shell git describe --tags --exact-match HEAD 2>/dev/null)

VERSION := $(if $(GIT_TAG),$(GIT_TAG),$(GIT_DATE)+$(GIT_HASH)_$(GIT_BRANCH))

CFLAGS  := -Wall -Wextra -O2 -Isrc -DVERSION='"$(VERSION)"' -DGIT_REMOTE='"$(GIT_REMOTE)"'

# K6: libcurl-impersonate is a drop-in libcurl with BoringSSL. It is not in
# Homebrew and the macOS release ships only executables, so the build has to be
# pointed at one explicitly:
#
#   make CURL_IMPERSONATE=1 CURL_IMPERSONATE_PREFIX=/usr/local/curl-impersonate
#
# Without it the binary reports "impersonation: off (stock libcurl)" and
# tests/test_k6_impersonation.sh proves the difference with a local listener.
CURL_IMPERSONATE ?= 0
CURL_IMPERSONATE_PREFIX ?= /usr/local
ifeq ($(CURL_IMPERSONATE),1)
  CFLAGS += -DHAVE_CURL_IMPERSONATE -I$(CURL_IMPERSONATE_PREFIX)/include
  LIBS   += -L$(CURL_IMPERSONATE_PREFIX)/lib -lcurl-impersonate
endif

ifeq ($(UNAME_S),Darwin)
BREW_PREFIX := $(shell brew --prefix 2>/dev/null || echo /opt/homebrew)
export PKG_CONFIG_PATH := $(BREW_PREFIX)/opt/openssl@3/lib/pkgconfig:$(BREW_PREFIX)/opt/curl/lib/pkgconfig:$(PKG_CONFIG_PATH)
DEP_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(PKG_DEPS) 2>/dev/null)
DEP_LIBS   := $(shell $(PKG_CONFIG) --libs $(PKG_DEPS) 2>/dev/null)
CFLAGS  += -I$(BREW_PREFIX)/include $(DEP_CFLAGS)
LIBS    += -L$(BREW_PREFIX)/lib -lbeaker $(DEP_LIBS) -lpthread -lm
else
CFLAGS  += -I/usr/include/libxml2
LIBS    += -lbeaker -lcurl -lxml2 -ljansson -lpthread -lm -lssl -lcrypto
endif

LDFLAGS :=

SRC_DIR := src
BIN_DIR := bin
OBJ_DIR := obj

# X2: a Finder duplicate ("Foo 1.c") is compiled as a second definition, and the
# filename contains a space, so the build fails in a way that reads like a
# mystery. Refuse to start rather than glob them in.
DUPE_SRCS := $(shell find $(SRC_DIR) -name '*.c' | grep -E ' [0-9]+\.c$$' || true)
ifneq ($(DUPE_SRCS),)
$(error duplicated source files in $(SRC_DIR) (Finder copies?): $(DUPE_SRCS))
endif
SRCS := $(shell find $(SRC_DIR) -name '*.c')
OBJS := $(SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

TARGET := $(BIN_DIR)/omnisearch

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(OBJS) -o $@ $(LDFLAGS) $(LIBS)
	@echo "Build complete: $(TARGET)"

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@
	@echo "Compiled: $<"

# VERSION reaches the compiler as -D, which make does not track: after a commit
# the version string in /info stayed at the previous build's hash until someone
# ran `make clean`. That is how a service can report a commit it is not running.
# A stamp file holding the version gives make something to watch, and only the
# three translation units that print it are rebuilt.
VERSION_STAMP := $(OBJ_DIR)/.version
$(VERSION_STAMP): FORCE
	@mkdir -p $(OBJ_DIR)
	@printf '%s' '$(VERSION)' | cmp -s - $@ 2>/dev/null || printf '%s' '$(VERSION)' > $@
$(OBJ_DIR)/Routes/Home.o $(OBJ_DIR)/Routes/History.o $(OBJ_DIR)/Routes/Info.o: $(VERSION_STAMP)

FORCE:

# header dependencies: without these a struct change silently mixed old object
# layouts with new ones
DEPS := $(OBJS:.o=.d)
-include $(DEPS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)
	@echo "Cleaned build artifacts"

rebuild: clean all

info:
	@echo "Compiler:    $(CC)"
	@echo "CFlags:      $(CFLAGS)"
	@echo ""
	@echo "Sources to compile:"
	@echo "$(SRCS)" | tr ' ' '\n'
	@echo ""
	@echo "Object files to generate:"
	@echo "$(OBJS)" | tr ' ' '\n'

ifeq ($(UNAME_S),Darwin)
PREFIX      ?= /usr/local
DATA_DIR    ?= $(PREFIX)/etc/omnisearch
CONF_DIR    ?= $(DATA_DIR)
VAR_DIR     ?= $(PREFIX)/var/lib/omnisearch
LOG_DIR     ?= $(PREFIX)/var/log/omnisearch
CACHE_DIR   ?= $(PREFIX)/var/cache/omnisearch
else
PREFIX      ?= /usr
DATA_DIR    ?= /etc/omnisearch
CONF_DIR    ?= /etc/omnisearch
VAR_DIR     ?= /var/lib/omnisearch
LOG_DIR     ?= /var/log/omnisearch
CACHE_DIR   ?= /var/cache/omnisearch
endif

INSTALL_BIN_DIR := $(PREFIX)/bin
USER        := omnisearch
GROUP       := omnisearch

SYSTEMD_DIR := /etc/systemd/system
RUNIT_DIR ?= $(error Please set RUNIT_DIR to your services directory)
OPENRC_DIR  := /etc/init.d
DINIT_DIR   := /etc/dinit.d
LAUNCHD_DIR ?= /Library/LaunchDaemons
LAUNCHD_LABEL ?= monster.bwaaa.omnisearch
LAUNCHD_PLIST := $(LAUNCHD_DIR)/$(LAUNCHD_LABEL).plist

install:
	@echo "Available install targets:"
	@echo "  make install-systemd"
	@echo "  make install-openrc"
	@echo "  make install-runit"
	@echo "  make install-s6"
	@echo "  make install-dinit"
	@if [ "$(UNAME_S)" = "Darwin" ]; then echo "  make install-launchd"; fi
	@echo ""
	@echo "Example: doas/sudo make install-openrc"

install-launchd: $(TARGET)
	@mkdir -p $(DATA_DIR)/templates $(DATA_DIR)/static $(DATA_DIR)/locales $(DATA_DIR)/tools $(INSTALL_BIN_DIR) $(LOG_DIR)
	@cp -rf templates/* $(DATA_DIR)/templates/
	@cp -rf static/* $(DATA_DIR)/static/
	@cp -rf locales/* $(DATA_DIR)/locales/
	@cp -rf tools/* $(DATA_DIR)/tools/
	@cp -n example-config.ini $(DATA_DIR)/config.ini || true
	install -m 755 $(TARGET) $(INSTALL_BIN_DIR)/omnisearch
	@mkdir -p $(LAUNCHD_DIR)
	@sed \
		-e 's|@INSTALL_BIN_DIR@|$(INSTALL_BIN_DIR)|g' \
		-e 's|@DATA_DIR@|$(DATA_DIR)|g' \
		-e 's|@LOG_DIR@|$(LOG_DIR)|g' \
		-e 's|@LAUNCHD_LABEL@|$(LAUNCHD_LABEL)|g' \
		init/launchd/omnisearch.plist.in > $(LAUNCHD_PLIST)
	@chmod 644 $(LAUNCHD_PLIST)
	@sed \
		-e 's|@DATA_DIR@|$(DATA_DIR)|g' \
		-e 's|@LAUNCHD_LABEL@|$(LAUNCHD_LABEL)|g' \
		init/launchd/logrotate.plist.in > $(LAUNCHD_PLIST:.plist=.logrotate.plist)
	@chmod 644 $(LAUNCHD_PLIST:.plist=.logrotate.plist)
	@echo ""
	@echo "Config: $(DATA_DIR)/config.ini"
	@echo "Installed launchd plist to $(LAUNCHD_PLIST)"
	@echo "Load with: sudo launchctl bootstrap system $(LAUNCHD_PLIST)"
	@echo "Enable with: sudo launchctl enable system/$(LAUNCHD_LABEL)"
	@echo "Start with: sudo launchctl kickstart -k system/$(LAUNCHD_LABEL)"

install-systemd: $(TARGET)
	@mkdir -p $(DATA_DIR)/templates $(DATA_DIR)/static $(DATA_DIR)/locales $(LOG_DIR) $(CACHE_DIR)
	@cp -rf templates/* $(DATA_DIR)/templates/
	@cp -rf static/* $(DATA_DIR)/static/
	@cp -rf locales/* $(DATA_DIR)/locales/
	@cp -n example-config.ini $(DATA_DIR)/config.ini || true
	install -m 755 $(TARGET) $(INSTALL_BIN_DIR)/omnisearch
	@echo "Setting up user '$(USER)'..."
	@(grep -q '^$(GROUP):' /etc/group || groupadd $(GROUP)) 2>/dev/null || true
	@id -u $(USER) >/dev/null 2>&1 || useradd --system --home $(DATA_DIR) --shell /usr/sbin/nologin -g $(GROUP) $(USER)
	@chown -R $(USER):$(GROUP) $(LOG_DIR) $(CACHE_DIR) $(VAR_DIR) $(DATA_DIR) 2>/dev/null || true
	@chown $(USER):$(GROUP) $(DATA_DIR)/config.ini 2>/dev/null || true
	install -m 644 init/systemd/omnisearch.service $(SYSTEMD_DIR)/omnisearch.service
	@echo ""
	@echo "Config: $(DATA_DIR)/config.ini"
	@echo "Edit config with: nano $(DATA_DIR)/config.ini"
	@echo "Installed systemd service to $(SYSTEMD_DIR)/omnisearch.service"
	@echo "Run 'systemctl enable --now omnisearch' to start"

install-openrc: $(TARGET)
	@mkdir -p $(DATA_DIR)/templates $(DATA_DIR)/static $(DATA_DIR)/locales $(LOG_DIR) $(CACHE_DIR)
	@cp -rf templates/* $(DATA_DIR)/templates/
	@cp -rf static/* $(DATA_DIR)/static/
	@cp -rf locales/* $(DATA_DIR)/locales/
	@cp -n example-config.ini $(DATA_DIR)/config.ini || true
	install -m 755 $(TARGET) $(INSTALL_BIN_DIR)/omnisearch
	@echo "Setting up user '$(USER)'..."
	@(grep -q '^$(GROUP):' /etc/group || groupadd $(GROUP)) 2>/dev/null || true
	@id -u $(USER) >/dev/null 2>&1 || useradd --system --home $(DATA_DIR) --shell /usr/sbin/nologin -g $(GROUP) $(USER)
	@chown -R $(USER):$(GROUP) $(LOG_DIR) $(CACHE_DIR) $(VAR_DIR) $(DATA_DIR) 2>/dev/null || true
	@chown $(USER):$(GROUP) $(DATA_DIR)/config.ini 2>/dev/null || true
	install -m 755 init/openrc/omnisearch $(OPENRC_DIR)/omnisearch
	@echo ""
	@echo "Config: $(DATA_DIR)/config.ini"
	@echo "Edit config with: nano $(DATA_DIR)/config.ini"
	@echo "Installed openrc service to $(OPENRC_DIR)/omnisearch"
	@echo "Run 'rc-update add omnisearch default' to enable"

install-runit: $(TARGET)
	@mkdir -p $(DATA_DIR)/templates $(DATA_DIR)/static $(DATA_DIR)/locales $(LOG_DIR) $(CACHE_DIR) $(RUNIT_DIR)/omnisearch/log/
	@cp -rf templates/* $(DATA_DIR)/templates/
	@cp -rf static/* $(DATA_DIR)/static/
	@cp -rf locales/* $(DATA_DIR)/locales/
	@cp -n example-config.ini $(DATA_DIR)/config.ini || true
	install -m 755 $(TARGET) $(INSTALL_BIN_DIR)/omnisearch
	@echo "Setting up user '$(USER)'..."
	@(grep -q '^$(GROUP):' /etc/group || groupadd $(GROUP)) 2>/dev/null || true
	@id -u $(USER) >/dev/null 2>&1 || useradd --system --home $(DATA_DIR) --shell /usr/sbin/nologin -g $(GROUP) $(USER)
	@chown -R $(USER):$(GROUP) $(LOG_DIR) $(CACHE_DIR) $(VAR_DIR) $(DATA_DIR) 2>/dev/null || true
	@chown $(USER):$(GROUP) $(DATA_DIR)/config.ini 2>/dev/null || true
	install -m 755 init/runit/run $(RUNIT_DIR)/omnisearch/run
	install -m 755 init/runit/log/run $(RUNIT_DIR)/omnisearch/log/run
	@echo ""
	@echo "Config: $(DATA_DIR)/config.ini"
	@echo "Edit config with: nano $(DATA_DIR)/config.ini"
	@echo "Installed runit service to $(RUNIT_DIR)/omnisearch"
	@echo "You need to start the service manually"
	@echo "Void: ln -s $(RUNIT_DIR)/omnisearch/ /var/service/"
	@echo "Artix: ln -s $(RUNIT_DIR)/omnisearch/ /run/runit/"

install-s6: $(TARGET)
	@mkdir -p $(DATA_DIR)/templates $(DATA_DIR)/static $(DATA_DIR)/locales $(LOG_DIR) $(CACHE_DIR)
	@cp -rf templates/* $(DATA_DIR)/templates/
	@cp -rf static/* $(DATA_DIR)/static/
	@cp -rf locales/* $(DATA_DIR)/locales/
	@cp -n example-config.ini $(DATA_DIR)/config.ini || true
	install -m 755 $(TARGET) $(INSTALL_BIN_DIR)/omnisearch
	@echo "Setting up user '$(USER)'..."
	@(grep -q '^$(GROUP):' /etc/group || groupadd $(GROUP)) 2>/dev/null || true
	@id -u $(USER) >/dev/null 2>&1 || useradd --system --home $(DATA_DIR) --shell /usr/sbin/nologin -g $(GROUP) $(USER)
	@chown -R $(USER):$(GROUP) $(LOG_DIR) $(CACHE_DIR) $(VAR_DIR) $(DATA_DIR) 2>/dev/null || true
	@chown $(USER):$(GROUP) $(DATA_DIR)/config.ini 2>/dev/null || true
	@mkdir -p /var/service/omnisearch/log/supervise/control
	install -m 755 init/s6/run /var/service/omnisearch/run
	install -m 755 init/s6/log/run /var/service/omnisearch/log/run
	install -m 755 init/s6/log/run /var/service/omnisearch/log/supervise/control
	@echo ""
	@echo "Config: $(DATA_DIR)/config.ini"
	@echo "Edit config with: nano $(DATA_DIR)/config.ini"
	@echo "Installed s6 service to /var/service/omnisearch"
	@echo "Service will start automatically"

install-dinit: $(TARGET)
	@mkdir -p $(DATA_DIR)/templates $(DATA_DIR)/static $(DATA_DIR)/locales $(LOG_DIR) $(CACHE_DIR)
	@cp -rf templates/* $(DATA_DIR)/templates/
	@cp -rf static/* $(DATA_DIR)/static/
	@cp -rf locales/* $(DATA_DIR)/locales/
	@cp -n example-config.ini $(DATA_DIR)/config.ini || true
	install -m 755 $(TARGET) $(INSTALL_BIN_DIR)/omnisearch
	@echo "Setting up user '$(USER)'..."
	@(grep -q '^$(GROUP):' /etc/group || groupadd $(GROUP)) 2>/dev/null || true
	@id -u $(USER) >/dev/null 2>&1 || useradd --system --home $(DATA_DIR) --shell /usr/sbin/nologin -g $(GROUP) $(USER)
	@chown -R $(USER):$(GROUP) $(LOG_DIR) $(CACHE_DIR) $(VAR_DIR) $(DATA_DIR) 2>/dev/null || true
	@chown $(USER):$(GROUP) $(DATA_DIR)/config.ini 2>/dev/null || true
	install -m 644 init/dinit/omnisearch $(DINIT_DIR)/omnisearch
	@echo ""
	@echo "Config: $(DATA_DIR)/config.ini"
	@echo "Edit config with: nano $(DATA_DIR)/config.ini"
	@echo "Installed dinit service to $(DINIT_DIR)/omnisearch"
	@echo "Run 'dinitctl enable omnisearch' to start"

uninstall:
	rm -f $(INSTALL_BIN_DIR)/omnisearch
	rm -rf $(DATA_DIR)
	rm -f $(SYSTEMD_DIR)/omnisearch.service
	rm -f $(OPENRC_DIR)/omnisearch
	rm -f $(DINIT_DIR)/omnisearch
	rm -rf /etc/service/omnisearch
	rm -rf /var/service/omnisearch
	@echo "You might need to unlink omnisearch if you're using runit"
	@id -u $(USER) >/dev/null 2>&1 && userdel $(USER) 2>/dev/null || true
	@grep -q '^$(GROUP):' /etc/group 2>/dev/null && groupdel $(GROUP) 2>/dev/null || true
	@echo "Uninstalled omnisearch"

check-themes:
	@python3 tools/check_themes.py

# A1: registry lint + parser fixtures. Neither touches the network.
# ApiEngines now fetches the Google CSE token itself, so the test builds need
# the HTTP helper and everything it stands on.
LINT_SRCS = src/Query/Tokens.c \
            src/Scraping/ScrapingParsers.c src/Scraping/BlockDetection.c \
            src/Scraping/ApiEngines.c src/Utility/Utility.c \
            src/Utility/XmlHelper.c src/Utility/Unescape.c \
            src/Utility/JsonHelper.c src/Utility/HttpClient.c \
            src/Proxy/Proxy.c src/Config.c src/Cache/Cache.c
# Config.c defines global_config, so anything linking Utility.c (which now asks
# it about the opt-in gate) needs Config.c as well.
# beaker has no .pc file; it lives in the brew prefix like the app build expects
LINT_LIBS = -L$(BREW_PREFIX)/lib -lbeaker $(DEP_LIBS) -lpthread -lm -lssl -lcrypto

tests/registry_lint: tests/registry_lint.c $(LINT_SRCS)
	@$(CC) -Isrc -I$(BREW_PREFIX)/include -o $@ $^ $(LINT_LIBS) -lcrypto \
		|| { echo "registry lint build failed"; exit 1; }

tests/parser_fixtures: tests/parser_fixtures.c $(LINT_SRCS)
	@$(CC) -Isrc -I$(BREW_PREFIX)/include -o $@ $^ $(LINT_LIBS) -lcrypto \
		|| { echo "fixture test build failed"; exit 1; }

registry-lint: tests/registry_lint
	@./tests/registry_lint

# the provider table claims to be group-ordered and the page relies on it
# Providers.c asks Reddit.c whether the OAuth app is configured, so both link in
LINT_SRCS_PROVIDERS = $(LINT_SRCS) src/Providers/Providers.c src/Scraping/Reddit.c \
                      src/Utility/Secrets.c src/Utility/Effective.c
tests/test_provider_order: tests/test_provider_order.c $(LINT_SRCS_PROVIDERS)
	@$(CC) -Isrc -I$(BREW_PREFIX)/include -o $@ $^ $(LINT_LIBS) -lcurl -lcrypto \
		|| { echo "provider order test build failed"; exit 1; }

provider-order-check: tests/test_provider_order
	@./tests/test_provider_order

# M4: the cx out of an id, a URL or an embed tag
tests/test_m4_cse: tests/test_m4_cse.c src/Scraping/CseId.c
	@$(CC) -Isrc -I$(BREW_PREFIX)/include -o $@ $^ || { echo "M4 test build failed"; exit 1; }

m4check: tests/test_m4_cse
	@./tests/test_m4_cse

# M1 gate, and X1/X4: every field of every card saves and clears
m1check:
	@./tests/test_m1_gate.sh
	@./tests/test_x4_fields.sh

fixtures: tests/parser_fixtures
	@./tests/parser_fixtures

tests/test_canonical: tests/test_canonical.c src/Ranking/Ranker.c \
                     src/Query/QueryAnalysis.c src/Utility/Utility.c \
                     src/Utility/Unescape.c src/Utility/XmlHelper.c \
                     src/Utility/Display.c src/Cache/Cache.c \
                     $(LINT_SRCS)
	@$(CC) -Isrc -I$(BREW_PREFIX)/include -I/usr/include/libxml2 -o $@ $^ \
		$(LINT_LIBS)

canonical: tests/test_canonical
	@./tests/test_canonical

config-check:
	@./tests/test_c15_config.sh

render-check:
	@./tests/test_template_balance.sh
	@./tests/test_context_capacity.sh

# K1.4: a toggle must change what leaves the machine. Needs a live server and
# a proxy that makes no requests, so it is not in the default check chain.
toggle-check:
	@./tests/test_k1_toggle_effect.sh

# G1: engines="*" must mean the registry default set, not "everything"
engines-check:
	@./tests/test_g1_engines.sh

# G2: the first-strike suspension table (no network, no server)
tests/test_g2_suspension: tests/test_g2_suspension.c src/Scraping/EngineHealth.c
	@$(CC) -Isrc -o $@ $^ -lpthread

suspend-check: tests/test_g2_suspension
	@./tests/test_g2_suspension

# G3: the Google CSE element parser and its token availability rule
tests/test_g3_cse_element: tests/test_g3_cse_element.c $(LINT_SRCS) \
                           src/Cache/Cache.c src/Scraping/ApiEngines.c
	@$(CC) -Isrc -I$(BREW_PREFIX)/include -o $@ $^ $(LINT_LIBS) -lcrypto

cse-check: tests/test_g3_cse_element
	@./tests/test_g3_cse_element

# G4: the answer-box table (local answerers only - no network needed)
tests/test_g4_answerbox: tests/test_g4_answerbox.c $(LINT_SRCS) \
                         src/Cache/Cache.c src/Config.c src/Proxy/Proxy.c \
                         src/Utility/JsonHelper.c src/Utility/HttpClient.c \
                         src/Ranking/Ranker.c src/Query/QueryAnalysis.c \
                         src/Infobox/AnswerBox.c src/Infobox/AnswerLocal.c \
                         src/Infobox/Calculator.c \
                         src/Infobox/Translate.c \
                         src/Infobox/Places.c src/Infobox/Stocks.c \
                         src/Infobox/UnitConversion.c \
                         src/Infobox/ColourCode.c \
                         src/Infobox/CurrencyConversion.c \
                         src/Infobox/Dictionary.c src/Infobox/Wikipedia.c \
                         src/Infobox/Infobox.c
	@$(CC) -Isrc -I$(BREW_PREFIX)/include -o $@ $^ $(LINT_LIBS) -lcrypto -lm

# K7: the fetch path against a local server - robots, the signed link, and the
# refusals (used by test_k7_fetch.sh, which owns the server)
k7fetch-check:
	@./tests/test_k7_fetch.sh

# K7: the description extractor and the robots reader, with no network
tests/test_k7_descriptions: tests/test_k7_descriptions.c \
                            src/Infobox/Descriptions.c src/Routes/ImageProxy.c \
                            $(LINT_SRCS) src/Cache/Cache.c src/Config.c \
                            src/Proxy/Proxy.c src/Utility/JsonHelper.c \
                            src/Utility/HttpClient.c src/Utility/Unescape.c \
                            src/Limiter/RateLimit.c
	@$(CC) -Isrc -I$(BREW_PREFIX)/include -o $@ $^ $(LINT_LIBS) -lcrypto -lm

k7check: tests/test_k7_descriptions
	@./tests/test_k7_descriptions

# K11: name -> symbol resolution, with no network (the bundled SEC table)
tests/test_k11_stocks: tests/test_k11_stocks.c src/Infobox/Stocks.c \
                       $(LINT_SRCS) src/Cache/Cache.c src/Config.c \
                       src/Proxy/Proxy.c src/Utility/JsonHelper.c \
                       src/Utility/HttpClient.c
	@$(CC) -Isrc -I$(BREW_PREFIX)/include -o $@ $^ $(LINT_LIBS) -lcrypto -lm

k11check: tests/test_k11_stocks
	@OMNI_STOCK_TABLE=static/stocks/company_tickers.tsv ./tests/test_k11_stocks

answer-check: tests/test_g4_answerbox
	@./tests/test_g4_answerbox

# H1: the UA ladder and the profile that survives a restart
tests/test_h1_ua: tests/test_h1_ua.c src/Scraping/EngineHealth.c \
                  src/Scraping/ScrapingHttp.c src/Scraping/Impersonate.c \
                  $(LINT_SRCS)
	@$(CC) -Isrc -I$(BREW_PREFIX)/include -o $@ $^ $(LINT_LIBS) -lcrypto -lm

ua-check: tests/test_h1_ua
	@./tests/test_h1_ua

# J3a: one tokenizer, and the queries it used to blank
tests/test_j3_terms: tests/test_j3_terms.c src/Query/Tokens.c \
                     src/Query/QueryAnalysis.c src/Ranking/Ranker.c \
                     src/Utility/Display.c src/Cache/Cache.c $(LINT_SRCS)
	@$(CC) -Isrc -I$(BREW_PREFIX)/include -I/usr/include/libxml2 -o $@ $^ \
		$(LINT_LIBS)

terms-check: tests/test_j3_terms
	@./tests/test_j3_terms

# J3d: the query is percent-encoded in every link
hrefs-check:
	@./tests/test_j3_hrefs.sh

# J5.6: the SearXNG bridge - bangs, and per-hit index groups
tests/test_j5_searxng: tests/test_j5_searxng.c src/Scraping/ApiEngines.c \
                       src/Utility/Display.c $(LINT_SRCS)
	@$(CC) -Isrc -I$(BREW_PREFIX)/include -I/usr/include/libxml2 -o $@ $^ \
		$(LINT_LIBS)

searxng-check: tests/test_j5_searxng
	@./tests/test_j5_searxng

# J5.1: an engine defined by a file, and a bad file refused at load time
tests/test_j5_defs: tests/test_j5_defs.c src/Scraping/EngineDefs.c $(LINT_SRCS)
	@$(CC) -Isrc -I$(BREW_PREFIX)/include -I/usr/include/libxml2 -o $@ $^ \
		$(LINT_LIBS)

defs-check: tests/test_j5_defs
	@./tests/test_j5_defs

# J1/J2: measured, not asserted. These need Playwright, which is deliberately
# not a dependency: without it they print a skip and exit 0.
layout-check:
	@$(PYTHON) tools/layout_check.py

theme-shots:
	@$(PYTHON) tools/theme_shot.py

# K9: the card is as wide as the keypad, at four widths
calc-card-check:
	@$(PYTHON) tools/calc_card_check.py

# the template context has 32 slots and beaker drops the 33rd silently
context-check:
	@./tests/test_context_budget.sh

# J4c/J4d: which queries ask for a place or a share price
tests/test_j4_places_stocks: tests/test_j4_places_stocks.c \
                         src/Infobox/Places.c src/Infobox/Stocks.c \
                         src/Utility/HttpClient.c src/Utility/Utility.c \
                         src/Utility/JsonHelper.c src/Utility/XmlHelper.c \
                         src/Utility/Unescape.c src/Config.c src/Cache/Cache.c \
                         src/Proxy/Proxy.c src/Scraping/ScrapingParsers.c \
                         src/Scraping/BlockDetection.c src/Scraping/ApiEngines.c
	@$(CC) -Isrc -I$(BREW_PREFIX)/include -I/usr/include/libxml2 -o $@ $^ \
		$(LINT_LIBS)

places-check: tests/test_j4_places_stocks
	@./tests/test_j4_places_stocks

# J4b: which queries are translation requests, and which are just searches
tests/test_j4_translate: tests/test_j4_translate.c src/Infobox/Translate.c \
                         src/Utility/HttpClient.c src/Utility/Utility.c \
                         src/Utility/JsonHelper.c src/Utility/XmlHelper.c \
                         src/Utility/Unescape.c src/Config.c src/Cache/Cache.c \
                         src/Proxy/Proxy.c src/Scraping/ScrapingParsers.c \
                         src/Scraping/BlockDetection.c src/Scraping/ApiEngines.c
	@$(CC) -Isrc -I$(BREW_PREFIX)/include -I/usr/include/libxml2 -o $@ $^ \
		$(LINT_LIBS)

translate-check: tests/test_j4_translate
	@./tests/test_j4_translate

# J5.3: category tabs - what they offer, what they route, and what they say
# when nothing serves the category
categories-check:
	@./tests/test_j5_categories.sh

# J5.5: the per-engine weight editor and the cookie behind it
weights-check:
	@./tests/test_j5_weights.sh

# K7 step 1: the placeholder table, and the merge rule that let it win
tests/test_k7_placeholders: tests/test_k7_placeholders.c src/Query/Placeholders.c
	@$(CC) -Isrc -o $@ $^

tests/test_k7_merge: tests/test_k7_merge.c src/Query/Placeholders.c
	@$(CC) -Isrc -o $@ $^

k7-check: tests/test_k7_placeholders tests/test_k7_merge
	@./tests/test_k7_placeholders
	@./tests/test_k7_merge

# J4.0: /w/<kind> serves one card, and the keypad protocol works through it
widgets-check:
	@./tests/test_widget_endpoints.sh

# J4.0/J4a: the calculator card, its keypad, and its states
widget-check:
	@./tests/test_j4_widget.sh

# J5.5 tier 3: the opt-in switch and its two gates
optin-check:
	@./tests/test_opt_in.sh
	@./tests/test_k1_optin.sh

# K12.6: an empty 200 is a block signal, not "no results"
k12-check:
	@./tests/test_k12_zero_body.sh

# K6: the binary says which libcurl it has; a local listener proves the
# ClientHello difference against a real browser, with no third party
k6-check:
	@./tests/test_k6_impersonation.sh

# K3: "Resumes" is an instant the dispatcher honours, not a rendered duration
k3-check:
	@./tests/test_k3_resume.sh

# L4: every unavailable row states a true reason and a fix; the bridges compose
l4check:
	@./tests/test_l4_reasons.sh

# L5: Reddit through the owner's own OAuth app, against a local stand-in
l5check:
	@./tests/test_l5_reddit.sh

# L6: the ids this project has shipped, and the placeholder filter with the
# description fetcher switched off
l6check:
	@./tests/test_l6_legacy_ids.sh
	@./tests/test_l6_placeholder_off.sh

# L3: the credentials form end to end - gate, 0600, masked, no leaks
l3accounts-check:
	@./tests/test_l3_accounts.sh

# L3: the secrets file - 0600, masked, env wins, cleared means gone
tests/test_l3_secrets: tests/test_l3_secrets.c src/Utility/Secrets.c
	@$(CC) -Isrc -o $@ $^ -lpthread

l3check: tests/test_l3_secrets
	@rm -rf /tmp/l3sec && mkdir -p /tmp/l3sec && chmod 700 /tmp/l3sec
	@L3_TMPDIR=/tmp/l3sec ./tests/test_l3_secrets

# L2: every id this project has shipped still works, and ddg-lite is gone
l2check:
	@./tests/test_l2_legacy.sh

# L1: the box means "on" everywhere, the no-script link works, and twenty
# providers switched off in one submission survive beaker's parameter limit
l1check:
	@./tests/test_l1_toggle.sh

# K4: the results page fits beaker's 32 context slots, with no key dropped
k4check:
	@./tests/test_k4_context.sh

# K4: the Videos tab and /videos (offline-safe: the "all off" page needs no
# network, and the card assertions only run when a provider answered)
k4videos-check:
	@./tests/test_k4_videos.sh

# K5.4: which queries start the Reddit lane, and what stops it (offline)
k5check:
	@./tests/test_k5_reddit_lane.sh

# J3c: results under the answer, and the three fan-out modes
answersfanout-check:
	@./tests/test_j3_answers.sh

# H4: the answer modes and the fan-out skip (starts its own instances)
answers-check:
	@./tests/test_h4_answers.sh

# H2: a JUNK answer is distinct from OK and EMPTY (fake upstream, no network)
junk-check:
	@./tests/test_h2_junk.sh

# A3: offline relevance benchmark. Reranks a recorded replay file with the
# production ranker - no network, no server, deterministic.
tests/offline_rank: tests/offline_rank.c src/Ranking/Ranker.c \
                    src/Query/QueryAnalysis.c src/Utility/Utility.c \
                    src/Utility/Unescape.c src/Utility/XmlHelper.c \
                    src/Utility/Display.c src/Cache/Cache.c \
                    $(LINT_SRCS)
	@$(CC) -Isrc -I$(BREW_PREFIX)/include -I/usr/include/libxml2 -o $@ $^ \
		$(LINT_LIBS)

REPLAY ?= tests/replay/baseline.jsonl

JUDGMENTS ?= tools/judgments.tsv

eval-offline: tests/offline_rank
	@./tests/offline_rank $(REPLAY) $(JUDGMENTS)

record-replay:
	@echo "Record with OMNI_REPLAY_RECORD=<file> ./bin/omnisearch, run the"
	@echo "queries in tools/queries.tsv against it, then copy the file to"
	@echo "tests/replay/."

smoke:
	@./tools/smoke.sh $(BASE)

eval:
	@python3 tools/eval.py $(BASE)

eval-strict:
	@python3 tools/eval.py $(BASE) --strict

# A7: one command that runs everything the project has, including a sanitizer
# build when asked. This is the substitute for the CI the project does not have.
verify: all check
	@echo "verify: all checks passed"

asan:
	@$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer \
		-g -o /tmp/omnisearch-asan $(SRCS) $(LDFLAGS) $(LIBS) \
		|| { echo "asan build failed"; exit 1; }
	@echo "asan binary: /tmp/omnisearch-asan"

dist:
	@git archive --format=tar.gz --prefix=omnisearch-$(VERSION)/ \
		-o /tmp/omnisearch-$(VERSION).tar.gz HEAD
	@echo "dist: /tmp/omnisearch-$(VERSION).tar.gz"
	@tar tzf /tmp/omnisearch-$(VERSION).tar.gz | grep -cE "^\./|obj/|bin/omnisearch$|\.DS_Store" \
		| grep -q '^0$$' && echo "dist: no build artefacts or junk" || echo "dist: WARNING artefacts present"
	@./tools/dist_check.sh /tmp/omnisearch-$(VERSION).tar.gz

check: check-themes registry-lint provider-order-check m4check m1check fixtures canonical config-check render-check engines-check suspend-check cse-check answer-check answersfanout-check widget-check searxng-check optin-check toggle-check k6-check k3-check k12-check terms-check hrefs-check defs-check categories-check context-check translate-check places-check weights-check k7-check k7check l1check l2check l3check l3accounts-check l4check l5check l6check k11check k4check k4videos-check k5check ua-check answers-check junk-check smoke

.PHONY: m1check m4check provider-order-check l5check l6check k12-check k3-check toggle-check terms-check hrefs-check answersfanout-check widget-check searxng-check optin-check all run clean rebuild info verify asan dist check check-themes registry-lint provider-order-check m4check m1check fixtures canonical config-check render-check engines-check suspend-check cse-check answer-check ua-check answers-check junk-check eval-offline record-replay smoke eval eval-strict install install-launchd install-systemd install-openrc install-runit install-s6 install-dinit uninstall
