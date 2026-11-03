# Local OpenSSL CPU-dispatch diagnostic

On the Intel Core Ultra 9 285K test host, the combined TLS combat/potion fixture
using pinned OpenSSL 3.6.0 reproducibly loses a nonvolatile XMM6 value during
external calls with normal CPU dispatch. The same saved test executable passes when started
with `OPENSSL_ia32cap=0`. The precise library routine remains under investigation;
this is not evidence that the hardware, database, or potion state is faulty.

CDB shows the test's valid store pointer held in XMM6 across external calls,
then an invalid XMM6 value passed as `this` to the inventory load operation.
The source test body does not write that register between those operations.
Disabling only AES, carry-less multiply, or the tested AVX paths did not fix
the saved reproducer. Logs and the unchanged reproducer are private under
`out/combat-*` and `out/build/windows/src/Release/chronicles-tests-combat-av.exe`.

For the current loopback development trial, use the explicit process fallback:

```powershell
.\tools\start_persistent.ps1 -RequireNamedLogin -WorldBootstrapDiagnostic `
  -WorldHandshakeDelayMs 2000 -PortableOpenSsl
```

The helper sets the override for the new server process and restores its own
environment afterward. It does not change the user or machine environment.
TLS certificate verification, ciphers, and protocol versions remain configured
as before. The CPU override mechanism is documented in the
[OpenSSL manual](https://docs.openssl.org/3.6/man3/OPENSSL_ia32cap/).

The portable path has not been load-tested and may reduce cryptographic
throughput. It is an opt-in local diagnostic configuration, not a permanent
dependency repair or proof of compatibility with normal CPU dispatch.

The unchanged reproducer passed all 60 selected gameplay/world tests with
this override: 58 passed, two opt-in database/capture cases skipped. Their
owners separately passed the isolated PostgreSQL and private capture checks.
The parallel NPC verification run reported 50 passed, two skipped, and no
failures across its 52 selected tests with the same override.
