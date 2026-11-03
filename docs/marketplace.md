# Player marketplace

The recovered demo's seven world requests (20116–20122) and seven replies (221–227)
implement fixed-price, whole-listing auctions in the local emulator. Auctions are scoped
to a realm and remain available across map changes. This is separate from trading posts,
wagons and trade vouchers.

`MarketCatalog` reads the hash-verified `ItemMarketAllowList`, `ItemMarketCategory` and
`Item` tables at startup. The current export allowlists 177 items: 172 equipment definitions
and five crafting materials. Missing tables disable the service. Registration refuses
unknown items, unsupported storage kinds, equipped equipment, polished equipment and
definitions with a binding rule. Runtime sockets and binding flags are not represented in
the current inventory model; this implementation does not claim support for trading such
instances when that model is extended.

## Contract

- Register transfers one unequipped equipment instance or a positive material quantity
  into escrow. The price is the total Gold cost of the listing. Equipment grade, durability
  and creation timestamp are retained. The seller receives a listing identity.
- Query supports public purchasable listings, the character's registered listings that still
  hold seller assets, and unclaimed outcomes. A listing whose seller side has been settled
  (item retrieved or proceeds claimed) leaves the registered list and remains in history.
  Main/subcategory filters and latest/ascending-price/descending-price
  sorting use stable listing-ID ties. Pages contain 1–100 rows; totals do not depend on the
  requested offset. Public listings exclude the requesting seller, sold and revoked rows,
  and deadlines at or before the current time.
- Purchase debits the buyer's Gold and records that buyer, with the entire item and sale
  price still in escrow. A second purchaser cannot win the same listing. Self-purchase is
  refused. No online seller session is required.
- Claim grants the buyer's item or the seller's proceeds independently. A revoked or
  expired listing returns its item to its seller. Each participant has a durable claim
  receipt. Repeated claims return `Market_AlreadyClaimed` and never emit another reward.
- Revoke removes an unsold, unexpired listing from sale; the seller then claims the item.
  Expiry is exclusive and derived from the persisted wall-clock deadline, so it continues
  across restarts without a live timer. Claim persists the expired state.
- Retrieve: the client's listing-details screen sends `MarketRevokeReq` for every listing on
  the registered tab, including its "Retrieve" action on an expired listing. When the listing
  is already closed and unsold (expired, or cancelled but not yet settled), the seller's
  revoke returns the escrowed item exactly like a seller claim: one atomic commit through the
  same durable seller receipt, an inventory update and then `MarketRevokeRes`. A sold
  listing refuses with `Market_NotSellRegisterCancel` (its proceeds use `MarketClaimReq`);
  a settled listing returns `Market_AlreadyClaimed`.
- Details expose an active listing's equipment/material payload and quantity, or a closed
  listing to either participant. History returns the latest 100 completed sales involving
  the requesting character, with its buy/sell side. Other characters cannot inspect closed
  listings or participant history.

Successful inventory/currency changes emit `PS2C_ItemsUpdateNoti` with committed instance
IDs, resulting material quantities, Gold deltas/balances and the request sequence, followed
by the operation's market reply. The generic `system` reason and ordering are emulator
choices; the descriptors have no market-specific inventory-update reason.

## Atomicity and persistence

Both `MemoryStore` and PostgreSQL commit the listing transition, gameplay revision,
escrow removal/grant, Gold change and claim receipt together. Storage independently checks
the allowed economic delta, immutable listing attributes, ownership, expected item
attributes and the expected listing snapshot. Revision conflicts, insufficient funds,
full inventory/stacks and losing purchase/claim races leave all assets unchanged.

PostgreSQL schema 24 adds `emulator_market`, query indexes and registration-key uniqueness.
It uses parameterized SQL and locks the acting character before the listing; settlement
does not need to lock a second character. The original item row leaves inventory during
registration. Claims allocate a new inventory instance ID while preserving its attributes.
Completed listings retain history and receipts. Character deletion refuses outstanding
seller or buyer assets; memory character IDs are never reused after deletion.

The character worker adopts a committed tutorial revision before another feature can write.
Database/notification exceptions close the session through the existing failure path, so
reconnection reloads persisted assets after an ambiguous commit outcome. Registration keys
contain a random connection scope and the packet sequence, never tokens or credentials.
The native transport already rejects duplicate/out-of-order sequences. A registration sent
with a new sequence or after reconnect is a new request: the recovered schema has no stable
client operation key. Purchase and claim retries remain safe across connections/restarts
because they name the durable listing.

## Policy and evidence limits

The following are explicit emulator policies, not recovered official rules: no registration
fee or sales tax, positive prices up to the supported Gold limit, no extra level requirement
or relisting cooldown, 100 outstanding seller listings, 100 history rows, periods of one day,
three days and a 60-second `RapidTest`, whole-listing purchase and separate item/proceeds
claims. `priceCustomOverride` and the observed `MarketRelistCooldownMin` catalog name do not
establish official price-band or cooldown semantics and are not applied speculatively.

Schemas/opcodes and catalog rows are static evidence. None of the reviewed official capture
corpus contains a market workflow. C++ storage/service checks and a disposable two-player TLS
smoke are synthetic verification. Original-client UI rendering, ordering, time-unit handling
and full marketplace acceptance require a separate live trial. No production database
migration, running-service restart, deployment or original-client trial is performed by
the implementation's test tools.

## Client retrieve evidence (2026-11-01)

An original-client screenshot showed "Retrieve" on an expired listing refused with the
`Market_NotSellRegisterCancel` text. Static analysis of the saved client database
(`dumps/ida_research/market-retrieve-20261101/FINDINGS.md`) shows the listing-details
primary action (`0x146329960`) sends `MarketRevokeReq` on the registered tab. When the
listing's details flag at offset 257 is set, the client skips the cancel confirmation. On
success, the `MarketRevokeRes` handler (`0x145F0DFC0`) shows system message 441 instead of
440, removes the listing from its local list, and the UI queries again. The flag's writer
and the localized message texts were not recovered. The fixed server has synthetic
coverage only; an original-client retrieve trial is still required.

## Verification commands

2026-10-26 Release validation: all nine non-PostgreSQL marketplace cases pass. The
PostgreSQL case passes against a disposable cluster, including store recreation, concurrent
purchase/claim races, stale-revision rollback and durable receipts. The two-player HTTP/TLS
smoke passes all seven requests and consecutive tutorial-cache updates. Six existing memory,
native auth/chat/guild/world and mail integrations pass; the isolated PostgreSQL runner also
passes and stops its cluster. The reference generator's 11 tests pass. This is not a full
emulator-suite run. TLS checks use the existing child-process `OPENSSL_ia32cap=0` diagnostic;
they do not establish normal OpenSSL CPU-dispatch behavior or original-client acceptance.

```powershell
cmake --build out/build-vehicle --config Release --target chronicles-market-tests chronicles-server
& ./out/build-vehicle/src/Release/chronicles-market-tests.exe
python tools/market_smoke_test.py ./out/build-vehicle/src/Release/chronicles-server.exe --openssl 'C:/Program Files/Git/usr/bin/openssl.exe'
```

`Market.PostgresSettlementPersistsAcrossStoreRecreation` requires
`CHRONICLES_TUTORIAL_TEST_DATABASE_URL` pointing to a disposable database; it applies the
idempotent migration there. The existing `tools/test_postgres.py` runner executes it when
the market test executable is available and stops its private cluster afterward.
