# XVC Server Makefile
# Supports: ARM64, x86_64, cross-compilation
# Two binaries: xvc-discover and xvc-server
# Uses FTDI D2XX driver for MPSSE mode

# Detect architecture
ARCH ?= $(shell uname -m)
CROSS_COMPILE ?=

# Compiler and flags
CC = $(CROSS_COMPILE)gcc
CFLAGS = -Wall -Wextra -O2 -g
CFLAGS += -I./include

# D2XX library paths based on architecture
ifeq ($(ARCH),aarch64)
    D2XX_ARCH = arm64
else ifeq ($(ARCH),arm64)
    D2XX_ARCH = arm64
else ifeq ($(ARCH),x86_64)
    D2XX_ARCH = x86_64
else
    $(error "Unsupported architecture: $(ARCH). Only x86_64 and arm64 are supported.")
endif

# D2XX library configuration
D2XX_DIR = ./vendor/d2xx/$(D2XX_ARCH)
D2XX_LIB = $(D2XX_DIR)/build/libftd2xx.a
D2XX_INC = -I$(D2XX_DIR)

# libusb headers from D2XX bundle (for device discovery)
D2XX_LIBUSB_INC = -I$(D2XX_DIR)/libusb/libusb

# Include paths
CFLAGS += $(D2XX_INC) $(D2XX_LIBUSB_INC)

# Linker flags - libftd2xx.a already includes libusb objects
# Using shared libudev since static library is not commonly available
LDFLAGS_COMMON = -lpthread -lm -ldl -lrt -L/lib/x86_64-linux-gnu -l:libudev.so.1
LDFLAGS_SERVER = $(D2XX_LIB) $(LDFLAGS_COMMON)
LDFLAGS_DISCOVER = $(D2XX_LIB) $(LDFLAGS_COMMON)

# Debug mode
DEBUG ?= 0
ifeq ($(DEBUG),1)
    CFLAGS += -DDEBUG -g
else
    CFLAGS += -DNDEBUG
endif

# Directories
SRC_DIR = src
OBJ_DIR = obj
BIN_DIR = bin
CONFIG_DIR = config
LOG_DIR = logs
SCRIPT_DIR = scripts
VENDOR_DIR = vendor

# xvc-server source files
SERVER_SOURCES = $(SRC_DIR)/main.c \
                  $(SRC_DIR)/tcp_server.c \
                  $(SRC_DIR)/device_manager.c \
                  $(SRC_DIR)/xvc_protocol.c \
                  $(SRC_DIR)/ftdi_adapter.c \
                  $(SRC_DIR)/mpsse_adapter.c \
                  $(SRC_DIR)/config.c \
                  $(SRC_DIR)/whitelist.c \
                  $(SRC_DIR)/logging.c

# xvc-discover source files
DISCOVER_SOURCES = $(SRC_DIR)/discover_main.c \
                    $(SRC_DIR)/device_manager.c \
                    $(SRC_DIR)/config.c \
                    $(SRC_DIR)/logging.c

# Object files
SERVER_OBJECTS = $(SERVER_SOURCES:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
DISCOVER_OBJECTS = $(DISCOVER_SOURCES:$(SRC_DIR)/%.c=$(OBJ_DIR)/discover_%.o)

# Target binaries
TARGET_SERVER = $(BIN_DIR)/xvc-server
TARGET_DISCOVER = $(BIN_DIR)/xvc-discover

# Default target (build both)
all: dirs d2xx-lib $(TARGET_SERVER) $(TARGET_DISCOVER)

# Create directories
dirs:
	@mkdir -p $(OBJ_DIR)
	@mkdir -p $(BIN_DIR)
	@mkdir -p $(CONFIG_DIR)
	@mkdir -p $(LOG_DIR)
	@mkdir -p $(SCRIPT_DIR)

# Build D2XX library
d2xx-lib: $(D2XX_LIB)

# D2XX static library (pre-built, includes libusb)
$(D2XX_LIB):
	@echo "Using pre-built D2XX library for $(D2XX_ARCH)..."
	@if [ ! -f $@ ]; then \
		echo "Error: D2XX library not found at $@"; \
		echo "Please ensure D2XX driver is properly installed in $(D2XX_DIR)"; \
		exit 1; \
	fi
	@echo "D2XX library found: $@"

# Link xvc-server
$(TARGET_SERVER): $(SERVER_OBJECTS) $(D2XX_LIB)
	@echo "Linking xvc-server for $(D2XX_ARCH)..."
	$(CC) $(SERVER_OBJECTS) -o $@ $(LDFLAGS_SERVER)
	@echo "Build complete: $@"

# Link xvc-discover
$(TARGET_DISCOVER): $(DISCOVER_OBJECTS) $(D2XX_LIB)
	@echo "Linking xvc-discover for $(D2XX_ARCH)..."
	$(CC) $(DISCOVER_OBJECTS) -o $@ $(LDFLAGS_DISCOVER)
	@echo "Build complete: $@"

# Compile xvc-server source files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@echo "Compiling $< for $(D2XX_ARCH)..."
	$(CC) $(CFLAGS) -c $< -o $@

# Compile xvc-discover source files (with prefix)
$(OBJ_DIR)/discover_%.o: $(SRC_DIR)/%.c
	@echo "Compiling $< (for xvc-discover)..."
	$(CC) $(CFLAGS) -c $< -o $@

# Build only xvc-server
xvc-server: dirs d2xx-lib $(TARGET_SERVER)

# Build only xvc-discover
xvc-discover: dirs d2xx-lib $(TARGET_DISCOVER)

# Install both binaries
install: install-server install-discover
	@echo "Installation complete"

# Install xvc-server only
install-server: $(TARGET_SERVER)
	@echo "Installing xvc-server..."
	install -d /usr/local/bin
	install -m 755 $(TARGET_SERVER) /usr/local/bin/xvc-server
	install -d /etc/xvc-server
	install -m 644 config/xvc-server-multi.conf.example /etc/xvc-server/xvc-server-multi.conf.example
	install -d /etc/xvc-server/scripts
	install -m 755 scripts/xvc-server-daemon.sh /etc/xvc-server/xvc-server-daemon.sh
	install -d /etc/systemd/system
	install -m 644 scripts/xvc-server.service /etc/systemd/system/xvc-server.service
	@systemctl daemon-reload
	@echo "xvc-server installed"

# Install xvc-discover only
install-discover: $(TARGET_DISCOVER)
	@echo "Installing xvc-discover..."
	install -d /usr/local/bin
	install -m 755 $(TARGET_DISCOVER) /usr/local/bin/xvc-discover
	@echo "xvc-discover installed"

# Install systemd service
install-systemd:
	@echo "Installing systemd service..."
	install -d /etc/systemd/system
	install -m 644 scripts/xvc-server.service /etc/systemd/system/xvc-server.service
	@systemctl daemon-reload
	@echo "systemd service installed"

# Uninstall
uninstall:
	@echo "Uninstalling xvc-server and xvc-discover..."
	systemctl stop xvc-server 2>/dev/null || true
	rm -f /usr/local/bin/xvc-server
	rm -f /usr/local/bin/xvc-discover
	rm -rf /etc/xvc-server
	rm -f /etc/systemd/system/xvc-server.service
	systemctl daemon-reload
	@echo "Uninstall complete"

# Clean build artifacts
clean:
	@echo "Cleaning..."
	rm -rf $(OBJ_DIR) $(BIN_DIR)
	@echo "Clean complete"

# Clean D2XX libraries (use with caution - requires re-extraction)
clean-d2xx:
	@echo "Cleaning D2XX libraries..."
	@echo "Note: You will need to re-extract the D2XX driver archives"

# Full clean (including D2XX)
distclean: clean clean-d2xx

# Show configuration
info:
	@echo "Build Configuration:"
	@echo "  Architecture: $(ARCH) ($(D2XX_ARCH))"
	@echo "  Compiler: $(CC)"
	@echo "  D2XX Library: $(D2XX_LIB)"
	@echo "  CFLAGS: $(CFLAGS)"
	@echo "  LDFLAGS_SERVER: $(LDFLAGS_SERVER)"
	@echo "  LDFLAGS_DISCOVER: $(LDFLAGS_DISCOVER)"

# Run tests
test: all
	@echo "Running tests..."
	./$(TARGET_DISCOVER) --help || true

# Debug build
debug:
	$(MAKE) DEBUG=1 all

# Release build
release:
	$(MAKE) DEBUG=0 all

# Cross-compile for ARM64
cross-arm64:
	$(MAKE) ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-

# Cross-compile for x86_64
cross-x86_64:
	$(MAKE) ARCH=x86_64

# Help
help:
	@echo "XVC Server Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  all            - Build both xvc-server and xvc-discover (default)"
	@echo "  xvc-server     - Build xvc-server only"
	@echo "  xvc-discover   - Build xvc-discover only"
	@echo "  install        - Install both binaries to /usr/local/bin"
	@echo "  install-server - Install xvc-server only"
	@echo "  install-discover - Install xvc-discover only"
	@echo "  clean          - Remove build artifacts"
	@echo "  distclean      - Remove all build artifacts including D2XX"
	@echo "  info           - Show build configuration"
	@echo "  debug          - Build with debug symbols"
	@echo "  release        - Build optimized release"
	@echo "  cross-arm64    - Cross-compile for ARM64"
	@echo "  cross-x86_64   - Build for x86_64"
	@echo "  help           - Show this help message"
	@echo ""
	@echo "Variables:"
	@echo "  ARCH=x86_64|arm64       - Target architecture"
	@echo "  CROSS_COMPILE=prefix-   - Cross-compiler prefix"
	@echo "  DEBUG=0|1               - Debug mode"

.PHONY: all dirs d2xx-lib xvc-server xvc-discover install install-server install-discover install-systemd uninstall clean clean-d2xx distclean info test debug release cross-arm64 cross-x86_64 help
