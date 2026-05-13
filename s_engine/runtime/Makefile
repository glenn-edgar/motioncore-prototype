# ============================================================================
# Makefile for S-Expression Engine Runtime Library
# Location: runtime/Makefile
# Builds: ../lib/libs_s_engine.a
# ============================================================================

CC = gcc
AR = ar
CFLAGS = -Wall -Wextra -std=c11 -O2 -g
CFLAGS += -DMODULE_IS_64BIT=0

# Directories
OBJ_DIR = obj
LIB_DIR = ../lib
INCLUDE_DIR = ../include/s_engine

# Auto-discover source and header files
SRCS = $(wildcard *.c)
HDRS = $(wildcard *.h)

# Object files
OBJS = $(SRCS:%.c=$(OBJ_DIR)/%.o)

# Output library
LIB = $(LIB_DIR)/libs_s_engine.a

# Default target
all: $(LIB) install_headers

# Create directories
$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(LIB_DIR):
	mkdir -p $(LIB_DIR)

$(INCLUDE_DIR):
	mkdir -p $(INCLUDE_DIR)

# Build static library
$(LIB): $(OBJS) | $(LIB_DIR)
	$(AR) rcs $@ $^
	@echo "Built: $@"

# Compile source files - rebuild on any header change
$(OBJ_DIR)/%.o: %.c $(HDRS) | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Install headers
install_headers: $(HDRS) | $(INCLUDE_DIR)
	cp $(HDRS) $(INCLUDE_DIR)/
	@echo "Installed headers to $(INCLUDE_DIR)"

# Clean
clean:
	rm -rf $(OBJ_DIR)

cleanall: clean
	rm -rf $(LIB_DIR) $(INCLUDE_DIR)

.PHONY: all clean cleanall install_headers