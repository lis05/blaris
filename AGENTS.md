# Rust Guidelines

## Minimal Derives
Do not add unnecessary `#[derive(...)]` attributes (e.g. `Debug`, `PartialEq`, `Eq`, `Hash`, `Default`) to structs or enums unless:
1. They are explicitly requested by the user.
2. They are strictly required by the compiler or existing code to build.
3. For unit test assertions, prefer testing specific fields or gating test derives (`#[cfg_attr(test, derive(...))]`) rather than unconditionally adding boilerplate derives to core domain types.
