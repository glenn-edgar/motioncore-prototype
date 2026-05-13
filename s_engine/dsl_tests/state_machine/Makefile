# ============================================================================
# Makefile for S-Expression Engine Demo/Test
# Location: output/Makefile
# Builds: s_expr_test
# ============================================================================

CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -O2 -g
CFLAGS += -DMODULE_IS_64BIT=0

# Directories
OBJ_DIR = obj
LIB_DIR = ../../lib
INCLUDE_DIR = ../../include/s_engine
RUNTIME_DIR = ../../runtime

# Include paths
CFLAGS += -I. -I$(INCLUDE_DIR)

# Auto-discover source and header files
SRCS = $(wildcard *.c)
HDRS = $(wildcard *.h)

# Library headers (for dependency tracking)
LIB_HDRS = $(wildcard $(INCLUDE_DIR)/*.h)

# Library file
LIB_FILE = $(LIB_DIR)/libs_s_engine.a

# Object files
OBJS = $(SRCS:%.c=$(OBJ_DIR)/%.o)

# Output executable
TARGET = main

# Default target
all: $(TARGET)

# Create directories
$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

# Build executable - depends on library
$(TARGET): $(OBJS) $(LIB_FILE)
	$(CC) $(CFLAGS) $(OBJS) -L$(LIB_DIR) -ls_s_engine -lm -o $@
	@echo "Built: $@"

# Compile source files - rebuild on any local or library header change
$(OBJ_DIR)/%.o: %.c $(HDRS) $(LIB_HDRS) | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build library if out of date (checks runtime sources)
$(LIB_FILE): $(wildcard $(RUNTIME_DIR)/*.c) $(wildcard $(RUNTIME_DIR)/*.h) $(LIB_HDRS)
	$(MAKE) -C $(RUNTIME_DIR)

# Force library rebuild
lib:
	$(MAKE) -C $(RUNTIME_DIR)

# Clean local
clean:
	rm -rf $(OBJ_DIR) $(TARGET)

# Clean all including runtime
cleanall: clean
	$(MAKE) -C $(RUNTIME_DIR) cleanall

# Rebuild everything
rebuild: cleanall
	$(MAKE) -C $(RUNTIME_DIR)
	$(MAKE)

# Run
run: $(TARGET)
	./$(TARGET)

.PHONY: all clean cleanall rebuild run lib