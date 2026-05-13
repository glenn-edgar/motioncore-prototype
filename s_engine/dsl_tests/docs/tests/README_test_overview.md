# S_Engine Test Overview

This document provides a comprehensive overview of the s_engine test suite. The tests validate the core functionality of the S-expression based execution engine, covering return codes, blackboard operations, dispatch mechanisms, state machines, predicates, sequences, loops, and JSON processing.

## Test Suite Organization

The test suite is organized into the following categories:

### Core Tests

**Return Codes** (`README_return_test.md`) — Validates the fundamental return code mechanism that drives s_engine execution flow. Tests verify that nodes correctly return SUCCESS, FAILURE, RUNNING, and other status codes that control tree traversal and execution decisions.

**Dispatch Test** (`README_dispatch.md`) — Tests the dispatch mechanism that routes execution to appropriate handlers based on node types. This validates the core execution model where S-expressions are evaluated and dispatched to registered handler functions.

### Blackboard Tests

The blackboard subsystem provides shared state storage for tree execution. These tests are located in the `black_board/` subdirectory:

**Blackboard Overview** (`black_board/README_test_overview.md`) — Overview of the blackboard testing strategy and architecture.

**DSL** (`black_board/README_DSL.md`) — Tests the domain-specific language constructs for blackboard access, including field references, assignments, and expressions.

**Blackboard Initialization** (`black_board/README_black_board_initializations.md`) — Validates proper initialization of blackboard data structures, memory allocation, and default value handling.

**Nested Field References** (`black_board/README_field_ref_nested_field_ref.md`) — Tests hierarchical field access patterns, validating that nested structures can be correctly referenced and modified.

**Main.c Integration** (`black_board/README_MAIN.md`) — Integration test demonstrating blackboard usage from the main C entry point.

### Control Flow Tests

**State Machine Test** (`README_state_machine.md`) — Validates state machine constructs within s_engine, testing state transitions, guard conditions, entry/exit actions, and hierarchical state handling.

**Loop Test** (`README_loop_test.md`) — Tests iteration constructs including counted loops, conditional loops, and collection iteration patterns.

**Complex Sequence Test** (`README_complex_sequence.md`) — Validates sequential execution patterns including error handling, early termination, and nested sequences.

### Predicate Tests

**Basic Predicate Test** (`README_basic_predicates.md`) — Tests fundamental predicate operations including comparisons, boolean logic, and type checks.

**Advanced Predicate Test** (`README_advanced_predicates.md`) — Tests complex predicate compositions, custom predicate functions, and predicate-based control flow.

### Data Processing Tests

**JSON Test** (`README_json.md`) — Validates JSON parsing, generation, and manipulation within the s_engine runtime, including blackboard integration for JSON data storage and retrieval.

