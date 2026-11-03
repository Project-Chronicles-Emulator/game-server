# Connecting another machine

Configurable API/world addresses, HTTPS/WSS and signed patch distribution are implemented.
The default configuration remains local at `http://127.0.0.1:8480` and world port `8481`.
See [distribution backend](distribution-backend.md) for the full setup and release workflow.

## What changes for a remote server

- Change `bind_address`, `public_address`, and `world_advertised_host` in the backend JSON config.
- Supply valid API and world TLS certificate chains/keys for those advertised names.
- Keep strict named-account launch grants enabled. Legacy Steam identity/shared-account mappings
  and the local TLS diagnostics are rejected on a remote listener.
- Generate a bootstrap template for the new API origin, publish a higher signed release sequence,
  and package the matching API origin/public signing key/channel in `launcher-config.json`.
- Keep PostgreSQL private to the server host. Only the configured API and world ports are player
  interfaces. The launcher never needs database credentials or the release signing private key.

## Evidence boundary

The backend and native updater are tested with disposable signed releases. Synthetic clients
exercise HTTPS password login, advertised launch grants, authenticated secure WebSockets and the
existing TLS world protocol checks. Original local-client milestones remain documented in
[launcher](launcher.md) and [validation](../VALIDATION.md).

Distribution profiles do not arm the loopback memory/TLS startup helper. The original client's
protected certificate/startup acceptance has not been validated against this remote path. A second
original client still needs a real trial with the chosen reachable host and trusted certificates;
a successful JSON login, signed patch installation or synthetic WebSocket test is not that proof.

## Original-client trial before public release

1. Deploy the strict HTTPS API and world TLS listener with the chosen DNS names and certificates.
2. Publish a curated matching client build and bootstrap template; install it into managed folders.
3. Use separate password-backed accounts and fresh launch grants on the two machines.
4. Verify native login, character ownership, world TLS, map load and mutual player visibility.
5. Verify session expiry, rejected cross-account operations and reconnect after a backend restart.

Do not claim public-player compatibility from schemas or synthetic integration checks alone.
