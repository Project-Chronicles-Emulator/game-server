# Publishing launcher updates

The player launcher checks the separately signed `launcher-stable` channel on startup. A newer
three-component launcher version is downloaded to a staging directory, verified against the pinned
Ed25519 key, and installed by a separate process after the launcher exits. The transactional patch
journal restores interrupted changes; settings, saved login data and game files are outside the
launcher release allowlist. The operator launcher does not use this channel.

This mechanism must first be installed manually: launchers through 0.2.4 cannot update themselves.
Distribute the first release containing this feature as a normal installer and portable ZIP. Do not
claim automatic upgrades are available until a packaged old-to-new trial and the published channel
have both been verified.

## Agent release procedure

1. Inspect the current production launcher channel, game channel, signing key identity, active
   release work and rollback artifacts. Coordinate with other active agents before publishing.
   Never replace the game channel or reset the shared release tree. Back up the current launcher
   channel manifest and retain its blobs. Keep the private signing key outside all served and
   player-visible directories.
2. Choose a strictly greater `major.minor.patch` version and increment the launcher channel's
   positive sequence. Update the CMake project version, package script, installer metadata, HTTP
   user agent and launcher UI version together. Never reuse a sequence for different content.
3. Build the release and run focused launcher, patch installer and publisher checks. Package using
   `tools/package_release.ps1` into a new directory. Review the staged `player/` tree: it must be
   the player edition, include the launcher, WebView2 loader, crypto DLL, world helper and UI,
   and contain no operator profile, credentials, settings, game files or captures. Include all
   files the new launcher needs, including its zone helper and runtime DLLs.
4. Exercise an installed and a portable copy using a disposable signed test service. Test a real
   old-to-new process handoff, restart, unchanged settings and game files, offline startup,
   corrupted downloads, concurrent/locked files and interrupted-commit recovery. Preserve the
   exact evidence; a successful package or synthetic signature check alone is not this trial.
5. Publish the reviewed player tree into a **separate staging release tree**, using the existing
   private signing key and the same public origin as production. For example:

   ```powershell
   python tools/publish_release.py --source <reviewed-player-directory> `
       --output <separate-staging-release-directory> --private-key <private-key-path> `
       --api-origin <production-api-origin> --channel launcher-stable `
       --version <new-version> --sequence <next-sequence> --no-profile
   ```

   Confirm the resulting public key matches the pinned player key and independently verify every
   staged blob's hash and size. The `--no-profile` option avoids overwriting the game's generated
   launcher configuration. Do not use a game bootstrap template for this channel.
6. Deploy additively: copy verified immutable blobs first, check them on the production host, and
   atomically replace only `channels/launcher-stable.json` last. Preserve the game `stable` channel,
   existing blobs and service configuration. Keep the prior launcher manifest for disaster recovery;
   normal releases must always advance the sequence. Confirm all existing production routes still
   respond and fetch the new signed manifest and every needed blob through the public API.
7. From a separate installation of the previous auto-update-capable release, verify the public
   download, process handoff, new version and preserved player data. Only then publish the installer,
   portable ZIP, checksums and release notes through the Project Chronicles Discord app, and read the post back.
   Clearly state when an original-client or load trial has not been performed.

The executable files are not code-signed with a Windows certificate; the updater's trust is the
signed release manifest and the pinned public key. A failed download leaves the current launcher in
place. An interrupted commit must be recovered before the launcher continues. A failure while the
game or another launcher instance holds files may require closing them and restarting the launcher.
The Windows uninstall entry can retain the version and file list from the original installer after
an automatic update; the player can use the latest installer when reinstalling or uninstalling.
