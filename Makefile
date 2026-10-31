CC      := cc
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

ifeq ($(UNAME_S),Darwin)
BREW_PREFIX := $(shell brew --prefix 2>/dev/null || echo /opt/homebrew)
export PKG_CONFIG_PATH := $(BREW_PREFIX)/opt/openssl@3/lib/pkgconfig:$(BREW_PREFIX)/opt/curl/lib/pkgconfig:$(PKG_CONFIG_PATH)
DEP_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(PKG_DEPS) 2>/dev/null)
DEP_LIBS   := $(shell $(PKG_CONFIG) --libs $(PKG_DEPS) 2>/dev/null)
CFLAGS  += -I$(BREW_PREFIX)/include $(DEP_CFLAGS)
LIBS    := -L$(BREW_PREFIX)/lib -lbeaker $(DEP_LIBS) -lpthread -lm
else
CFLAGS  += -I/usr/include/libxml2
LIBS    := -lbeaker -lcurl -lxml2 -ljansson -lpthread -lm -lssl -lcrypto
endif

LDFLAGS :=

SRC_DIR := src
BIN_DIR := bin
OBJ_DIR := obj

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
	@./tests/test_context_capacity.sh

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
                         src/Infobox/UnitConversion.c \
                         src/Infobox/ColourCode.c \
                         src/Infobox/CurrencyConversion.c \
                         src/Infobox/Dictionary.c src/Infobox/Wikipedia.c \
                         src/Infobox/Infobox.c
	@$(CC) -Isrc -I$(BREW_PREFIX)/include -o $@ $^ $(LINT_LIBS) -lcrypto -lm

answer-check: tests/test_g4_answerbox
	@./tests/test_g4_answerbox

# H1: the UA ladder and the profile that survives a restart
tests/test_h1_ua: tests/test_h1_ua.c src/Scraping/EngineHealth.c \
                  src/Scraping/ScrapingHttp.c $(LINT_SRCS)
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

# J4.0/J4a: the calculator card, its keypad, and its states
widget-check:
	@./tests/test_j4_widget.sh

# J5.5 tier 3: the opt-in switch and its two gates
optin-check:
	@./tests/test_opt_in.sh

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

check: check-themes registry-lint fixtures canonical config-check render-check engines-check suspend-check cse-check answer-check answersfanout-check widget-check searxng-check optin-check terms-check hrefs-check ua-check answers-check junk-check smoke

.PHONY: terms-check hrefs-check answersfanout-check widget-check searxng-check optin-check all run clean rebuild info verify asan dist check check-themes registry-lint fixtures canonical config-check render-check engines-check suspend-check cse-check answer-check ua-check answers-check junk-check eval-offline record-replay smoke eval eval-strict install install-launchd install-systemd install-openrc install-runit install-s6 install-dinit uninstall
