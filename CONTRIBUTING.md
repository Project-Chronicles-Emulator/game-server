# Contribute to Project Chronicles

[Project home](README.md) · [Setup](docs/guides/getting-started.md) · [Progress](docs/progress.md)

Good contributions make one behavior easier to understand, reproduce and maintain.
Gameplay fixes, extraction improvements, tests, documentation and precise bug reports
all help. Start small enough that another person can review the complete contract.

## Before you change code

1. Get the [local environment](docs/guides/getting-started.md) running.
2. Read the relevant [feature status and limits](docs/progress.md).
3. Find the owning module with the [architecture/source map](docs/guides/architecture.md).
4. Read its source, tests and evidence before choosing an implementation.
5. For substantial work, open an issue describing the intended behavior and scope.

Choose the repository that owns the change:

| Change | Repository |
| :--- | :--- |
| Authentication, sessions, grants or authority listener | [login-server](https://github.com/Project-Chronicles-Emulator/login-server) |
| Characters, gameplay, storage or world/native game protocol | [game-server](https://github.com/Project-Chronicles-Emulator/game-server) |
| Player desktop UI, client startup or distribution client | [launcher](https://github.com/Project-Chronicles-Emulator/launcher) |
| Operator desktop UI, connection management or proxy | [admin](https://github.com/Project-Chronicles-Emulator/admin) |

Shared auth/desktop libraries are currently versioned source copies. If a contract
changes across repositories, identify all consumers, make coordinated changes and
validate the pair. Do not create a build-time include dependency on a sibling checkout.

## Development path

Fork the relevant repository, create a focused branch and follow the
**[feature and bug-fix workflow](docs/guides/feature-workflow.md)**. It covers contract
definition, evidence, persistence, concurrency, tests and protocol documentation.
Consult [AGENTS.md](AGENTS.md) for the repository's code-quality rules; they apply to
handwritten code regardless of which editor or assistant is used.

Write readable C++20 with explicit ownership and failure paths. Follow the existing
formatter, use one statement per line and braces for conditional/loop bodies. Prefer
the owning feature/service over unrelated additions to the main world session.
Use existing libraries, parameterized SQL, transactions and revision guards.

## What belongs in a pull request

- The concrete trigger, previous behavior and resulting behavior.
- Scope and remaining limitations, including any build-specific or Candidate behavior.
- Relevant checks actually run, with a distinction between synthetic and client-observed results.
- Updated feature coverage, packet overrides and generated reference when behavior changes.
- A reproducible regression case when it verifies a meaningful state/ordering contract.

Keep unrelated formatting, generated assets, dependency upgrades and large refactors
out of a focused bug fix. Do not hand-edit generated protocol code or reference outputs.
Use the [testing guide](docs/guides/testing.md) to choose checks appropriate to the change.

## What must stay private/local

Do not submit game binaries/assets, archive keys, decoder binaries, raw packet
captures, database dumps, session/player values, chat logs, access-bearing URLs,
certificates or credentials. Sanitize examples and preserve build/source hashes
needed to understand the evidence. Report a sensitive problem without publishing
its credentials or exploitation details; ask maintainers for a private channel.

The public repository does not supply a client or archive key. Preserve upstream
dependency notices and submit only code/documentation you are entitled to contribute.

## Reporting a bug

Use the bug-report template. Include the component, commit, client/data identity,
smallest reproduction, expected/actual result and redacted error context. A screenshot
alone rarely identifies a server-side cause. If the original client was not used,
say so; synthetic reproductions are useful when labeled correctly.

## Release wording

Technical PRs can describe architecture, storage and tests. Player-facing patch notes
must describe only released, reachable changes players can see or do. A documentation
or infrastructure-only change has no new gameplay changelog.
