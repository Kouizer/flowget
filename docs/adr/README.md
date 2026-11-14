# Architecture Decision Records

One file per irreversible-or-expensive decision. Format: context, decision, consequences,
alternatives. Status is one of `Proposed`, `Accepted`, `Superseded by ADR-XXXX`.

| ADR | Title | Status |
|---|---|---|
| [0001](0001-http-stack.md) | HTTP stack: libcurl multi interface | Accepted |
| [0002](0002-c-abi-contract.md) | The C ABI is the only integration boundary | Accepted |
| [0003](0003-database-ownership.md) | SQLite is owned exclusively by the C++ core | Accepted |
| [0004](0004-third-party-licensing.md) | Third-party licensing policy | Accepted |
| [0005](0005-license-and-name.md) | License MIT, product name FlowGet | Accepted |
| [0006](0006-android-media-pipeline.md) | Android media pipeline | Accepted |

## When to write one

Write an ADR when a choice is hard to reverse (wire format, storage owner, license, background
execution model) or when a future contributor would reasonably ask "why on earth". Do not write one
for naming a variable.
