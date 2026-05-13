# Blackboard Test Overview

The blackboard subsystem provides shared state storage for s_engine tree execution. These tests validate the complete blackboard functionality including initialization, field access, nested references, DSL constructs, and user-defined functions.

## Test Categories

### DSL (`README_DSL.md`)

Documentation for writing blackboard constructs using the domain-specific language. Covers the S-expression syntax for field access, assignment expressions, conditional reads, and type-safe value manipulation. The DSL provides a declarative interface for tree nodes to interact with shared state without direct memory management.

### Main Integration (`README_MAIN.md`)

Documentation for runtime initialization of the blackboard from the main C entry point. Covers the complete initialization sequence, handler registration, tree loading, and configuring shared blackboards across multiple trees for coordinated state management.

### User Functions (`README_USER_FUNCTIONS.md`)

Documentation for writing user-defined virtual C functions and registering them in the system. Covers function implementation patterns, parameter passing from S-expressions, return value handling, and the registration process for integrating custom domain-specific operations into tree execution.

### Blackboard Initialization (`README_black_board_initializations.md`)

Documentation for initializing blackboards with constant data at startup. Covers memory allocation, field registration, default value assignment, and type configuration for pre-populating blackboard structures with known values before tree execution begins.

### Nested Field References (`README_field_ref_nested_field_ref.md`)

Documentation for creating nested fields within blackboard structures. Covers hierarchical field access patterns, dot-notation syntax, array indexing, nested structure traversal, and reference resolution across multiple levels. This enables structured data organization within the blackboard for complex control applications.

## Blackboard Architecture

The blackboard serves as a shared memory space accessible by all nodes during tree execution. Key characteristics:

- **Type Safety** — Fields are typed and validated during access
- **Hierarchical Organization** — Supports nested structures and arrays
- **Transactional Updates** — Changes can be committed or rolled back
- **Observable** — Nodes can register for change notifications
- **Persistent** — State survives across tree ticks when configured

## Related Documentation

- Main s_engine test overview for complete test suite organization
- S-expression DSL reference for blackboard access syntax
- Blackboard API reference for C integration