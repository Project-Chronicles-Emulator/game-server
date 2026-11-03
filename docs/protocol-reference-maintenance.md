# Protocol reference ownership and maintenance

The maintained cloud index is
[Project Chronicles protocol reference](https://chatgpt.com/space/page_d76bbd1a8b988191a91ed6afa2af9adf).
Its exact Page ID is `page_d76bbd1a8b988191a91ed6afa2af9adf`. Eight child Pages contain
the complete reference, divided at feature boundaries to stay below the service's 1 MiB
document limit. They retain private access; sharing permissions were not changed.
`docs/protocol-reference-publication.json` records the ordered child IDs, URLs and first
headings. The single local Markdown file remains canonical.

The canonical local reference is [protocol-feature-reference.md](protocol-feature-reference.md).
Agents must consult its relevant feature section and owning source before changing behavior.
The reference describes the recovered demo build, not unknown packets or future builds.

## Owning inputs

- `protocol/feature-coverage.json`: handwritten feature descriptions, current scope,
  missing work, verification limits, source/test pointers, packet status overrides,
  classification rules, known bodyless HTTP routes, and reviewed-source hashes.
- `protocol/demo-capture-observations.json`: sanitized packet counts, levels and artifact
  hashes for the capture corpus, plus sanitized comparisons against earlier captures.
  No private packet values belong in this file.
- `protocol/opcodes.json` and `protocol/native-ws-opcodes.json`: recovered numeric IDs.
  Keep service namespaces separate and never infer IDs for unresolved wrappers.
- `protocol/descriptors.normalized.pb`: authoritative payload field numbers and types.
- `tools/build_protocol_reference.py`: generates the Markdown and machine-readable catalog.
- `docs/protocol-reference-publication.json`: Page identity and last verified local document
  hash. It records a confirmed publication, rather than automatically pretending to sync.

Generated outputs are `docs/protocol-feature-reference.md`,
`docs/protocol-packet-catalog.json`, and ignored `out/protocol-reference/page-blocks.json`.
Do not hand-edit generated output. Update its owning input or generator instead.

## Required workflow for a feature change

1. Read the feature section, relevant packet entries, current implementation and owning
   validation record. Check newer source/captures before repeating an older missing claim.
2. Make the authorized implementation change, preserving unrelated work. Update the same
   feature's scope, packet overrides, remaining work, dependencies, and actual verification
   evidence in `protocol/feature-coverage.json`. Add newly relevant source paths to
   `reviewed_sources`. For pure refactors, record the unchanged contract and checks.
3. Inspect the changed source and diff. Only then record the review and regenerate:

   ```powershell
   python tools/build_protocol_reference.py --refresh-reviewed-sources
   ```

   Refreshing hashes does not infer coverage or verify correctness. Do not use it to bypass
   inspection or erase an unresolved drift warning. Status changes require behavioral evidence.

4. Synchronize the child Pages from the generated content. Partition the generated document
   at the ordered first headings recorded in the publication manifest; the last part includes
   the provenance section. Keep each part below 1 MiB. First use `read_page` on each exact
   Page ID with the relevant feature title or observed block IDs. Retain returned guidance,
   block IDs, hashes and optional sequence guards. Use guarded edits for the changed content;
   do not replace someone else's concurrent changes. For a full regeneration, compare all
   changed sections and totals, then update only the corresponding blocks. Read back the
   affected content and verify that every packet/feature change and updated totals survived.
   Confirm the root index links to every child in order. Verify the concatenated child
   content against the complete local document; compare nonblank lines when the Page parser
   changes paragraph spacing. Never substitute a summary for any packet or evidence section.
5. After confirmed readback of every affected part, update `document_sha256` in
   `docs/protocol-reference-publication.json` to the SHA-256 of the local generated Markdown.
   Update `verified_at_utc`, ordered part records and retain the exact index Page ID/URL.
   Never update this record merely
   because a Page write was attempted or its outcome was ambiguous.
6. Finish with:

   ```powershell
   python tools/build_protocol_reference.py --check
   ```

   This checks source-review fingerprints, complete inventory construction, capture-count
   consistency, current generated outputs and the last verified Page publication fingerprint.
   It does not contact the Page service or prove no one edited the Page afterward.

If Page access is unavailable, finish useful local changes and explicitly report that the
Page mirror is stale. Keep the last confirmed publication hash unchanged, so the check
continues to expose that gap. Do not block unrelated authorized implementation work on
cloud availability or claim the entire documentation workflow passed.

If unrelated runtime edits are still awaiting their owning source review, a capture-only
evidence publication may use `python tools/build_protocol_reference.py --allow-pending-source-reviews`.
This explicitly publishes the outstanding paths and leaves their review hashes untouched.
It cannot be combined with `--check` or `--refresh-reviewed-sources`; the strict check
must continue failing until the owning review is complete. Report that limitation when
publishing. This option does not certify those runtime changes or authorize status promotion.

## Evidence rules

Runtime labels are implemented, partial or missing for a stated bounded contract. A feature
can be partial while a specific request is implemented. Recovered schemas, generated classes,
decoded official captures, source tests and original-client local observations are distinct.
An ignored/rejected request is not feature implementation. A successful official request is
not proof of emulator compatibility. Empty initial snapshots do not implement a subsystem.

Payload field names are safe protocol metadata; player names, runtime character/item IDs,
chat text, session tokens, credentials and access-bearing URLs are private. Keep raw and
decoded captures under ignored `out/`; only sanitized static counts and provenance may be
published. Extend capture observations only after verifying their build and input hashes.
