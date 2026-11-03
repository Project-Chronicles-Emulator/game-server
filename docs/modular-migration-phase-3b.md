# Phase 3b: native world transport ownership

The native world listener delegates TLS setup, bind/accept and transport stop to
`net/world_tls_listener.*`. `net/world_connection.*` owns the shared TLS context,
stream, diagnostic delay, credential buffers, framed reads, bounded encoded writes
and existing read-resume gate. Metadata-only OpenSSL callbacks live in
`net/world_tls_diagnostics.*`.

The public native listener factory and application policy remain in world/session.
Authentication, admission, worker dispatch, gameplay timers, feature state, protocol
processing and close-time persistence stay above net. Net has no world, auth,
persistence or feature dependency.

## Preserved boundaries

One shared object owns the connection and its derived world session. The transport
is the sole `enable_shared_from_this` base; existing worker closures retain that
same derived session. TLS diagnostic storage outlives the stream's destruction.
No virtual hook runs from a destructor.

The accept callback is owned by the pending asynchronous operation, then transferred
to the next accept. It is not a permanent callback field that would create a
listener ownership cycle. Stopping the listener preserves the previous
accept-only stop behavior; this change does not introduce active-session draining.

Handshake and credential reads retain their existing timeout and buffer boundaries:
15 seconds for TLS and the 32-byte prefix, two seconds for the optional 68-byte
native tail, and 90 seconds for normal frame I/O. Diagnostic peeking consumes no
credential bytes. The application decides authentication and native-tail ownership.
Credential buffers remain stable until that phase's worker posts its continuation.

Incoming frames retain the two-byte little-endian inclusive length and eight-byte
minimum client header. The length is checked before allocation. Read and write state
stays on the existing I/O executor, with at most one read and one write in flight.
Queue accounting and in-flight shared-payload ownership retain their previous rules.

The worker returns a batch ordered as handshake response followed by notifications.
I/O queues that batch and chooses the existing drain or fixed-barrier read policy
using the then-current presence state. Later notifications do not extend a fixed
barrier. An empty result resumes reading; a rejected result closes the connection.
The completed-write hook still handles world exit before presence publication and
before read resumption. Close is idempotent, sets the closed flag first, and retains
application cleanup/persistence before physical socket closure.

Two construction details are explicit. Connection IDs are now allocated before base
stream construction; a failed constructor can leave an extra unused opaque identity.
IDs remain registry/hub keys, with no gapless-allocation contract. Also, shared world
state is constructed before listener bind/TLS-file validation, so simultaneous startup
errors can be reported in a different order. Successful startup still validates all
inputs before the factory returns. Neither detail changes admission or packet policy.

## Verification

The actual main-checkout net library/tests, world-session tests and server linked
successfully. The remaining production and test targets were then rebuilt/relinked.
The complete CTest run had 1,526 passes, 119 environment-gated skips and zero failures
in 75.38 seconds. All 1,613 prior unique names remain; exactly 32 were added: 15 listener
and 17 connection cases. Existing native-world and read-gate test files were unchanged.

The focused application/transport gate passed all 52 cases with no skips in 19.54
seconds: 41 net cases and 11 native admission, capacity, presence and disconnect cases.
Its catalog opt-in read the existing generated catalog. The actual server also passed
all 39 synthetic native-world smoke checks. Normal OpenSSL CPU dispatch was used.
These are synthetic/loopback checks, not new original-client acceptance.

The module checker reports zero violations, and new-source formatting/whitespace
checks pass. The new net production code matches the independently reviewed candidate.
The production binding differs from its reviewed snapshot only by obsolete includes
and eight substitutions of Beast's error-code alias with the identical Boost.System
type. The earlier private 26-case probe run and binding object compile remain supporting
evidence; the main-checkout results above establish the actual application integration.

Private receipts and test-name accounting are under ignored
`out/modular-plan/phase3b-integration` and the isolated checkout's
`out/modular-plan/connection-extraction/MAIN_INTEGRATION.md`. No deployment, live database
change or active-session drain was performed.

Build measurements belong in `docs/build-performance.md`. Existing original-client
acceptance limits and the unresolved fixture-enabled baseline failures documented in
`docs/modular-migration-phase-3a.md` remain separate from this structural change.
