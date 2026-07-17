# Signing the macOS build (Gatekeeper / notarization)

macOS has an equivalent of Windows SmartScreen: **Gatekeeper**. An unsigned or
merely ad-hoc-signed `.app` downloaded from the internet is blocked with *"bsdrX
can't be opened because it is from an unidentified developer"* (or, on recent
macOS, *"…Apple could not verify bsdrX is free of malware"*).

There are **two distinct signatures** at play, and they solve different problems:

| Signature | What it does | Cost |
| --- | --- | --- |
| **Ad-hoc** (`codesign -s -` / `rcodesign sign`) | Lets the **arm64** slice *execute* on Apple Silicon. Does **not** clear Gatekeeper. | free |
| **Developer ID + notarization** | Clears Gatekeeper — no prompt on download. | **$99/yr** Apple Developer Program |

`scripts/build-osx-bundle.sh` already **ad-hoc signs** every build (via `rcodesign`,
which it self-provisions — so the arm64 build runs on Apple Silicon out of the box).
Developer ID signing + notarization is **optional** and off unless you configure it.

## Is there a free path? (Short answer: no)

Unlike Windows — where SignPath Foundation gives OSS projects a free cert — **Apple
has no free code-signing/notarization program.** To silence Gatekeeper you need a paid
**Apple Developer Program** membership ($99/year) for a *Developer ID Application*
certificate. That is the only route.

Free workarounds that avoid the *cost* but not the *friction* (users must act):

- **Right-click → Open** the first time (then macOS remembers the choice).
- Strip the quarantine attribute: `xattr -dr com.apple.quarantine /Applications/bsdrX.app`.
- Ship as a plain CLI binary run from Terminal (Gatekeeper is laxer for non-quarantined
  command-line tools), rather than a double-clickable `.app`.

None of these remove the warning for a user who just downloads and double-clicks — only
Developer ID + notarization does that.

## Real signing + notarization (all from Linux, via rcodesign)

The whole flow works **without a Mac** — `rcodesign` (already in this repo's build
image) signs and notarizes from Linux. One-time setup:

1. Join the **Apple Developer Program** ($99/yr).
2. Create a **Developer ID Application** certificate and export it (with its private
   key) as a `.p12`. On a Mac: Keychain Access → export. From Linux you can generate
   the CSR with `rcodesign` and download the issued cert from developer.apple.com, then
   bundle key+cert into a `.p12`.
3. Create an **App Store Connect API key** (Individual/Admin role) for notarization and
   encode it for rcodesign:
   ```sh
   rcodesign encode-app-store-connect-api-key \
     -o /secrets/notary.json <ISSUER_ID> <KEY_ID> /path/AuthKey_<KEY_ID>.p8
   ```

Then just set the env vars before the bundle build — the script does the rest:

```sh
export OSX_CODESIGN_P12=/secrets/developer_id.p12
export OSX_CODESIGN_P12_PASS='…'          # optional
export OSX_NOTARY_API_KEY=/secrets/notary.json   # omit to sign but not notarize
bash scripts/build-osx.sh                  # or however you invoke the osx bundle
```

Output when configured:

```
>> ad-hoc signed bsdr_agent via rcodesign …      (always)
>> Developer ID signed bsdrX.app
>> notarized + stapled bsdrX.app (Gatekeeper will pass silently)
```

- Signing uses `--code-signature-flags runtime` (the **hardened runtime**, a
  notarization prerequisite). `rcodesign` walks the bundle and signs the nested
  `libonnxruntime` dylib automatically.
- `--staple` attaches the notarization ticket to the `.app` so it validates **offline**
  (no round-trip to Apple on first launch).

Unconfigured, step 3c is skipped — the app still builds and runs (ad-hoc), it just
prompts on download. See also `docs/WINDOWS-SIGNING.md` for the Windows equivalent.
