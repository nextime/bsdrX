# Signing the Windows build (Authenticode / SmartScreen)

The Windows `.exe` and its NSIS `Setup.exe` are **unsigned by default**. Unsigned
binaries trigger Windows SmartScreen — the *"Windows protected your PC — unknown
publisher, Run anyway?"* dialog. Signing them with a real code-signing certificate
removes (or, for OV certs, eventually removes) that prompt.

`scripts/build-win-bundle.sh` signs both PE files through one helper, `sign_pe()`,
which supports three mutually-exclusive modes, selected by environment variables in
this order. **Unconfigured, signing is a no-op** — the bundle still builds and ships,
it just keeps warning. Set the variables in your shell (or CI secrets) before running
`WIN_DEPS=… bash scripts/build-win-bundle.sh`.

| Env var (leading one wins) | Mode |
| --- | --- |
| `WIN_CODESIGN_CMD` | **1 — external signer**: any CLI that signs a file in place (cloud eSigner, KeyLocker). No osslsigncode needed. |
| `WIN_CODESIGN_PKCS11_MODULE` | **2 — PKCS#11**: hardware token or cloud HSM via `osslsigncode -pkcs11module`. |
| `WIN_CODESIGN_PFX` | **3 — local .pfx/.p12**: a file-based cert (or self-signed, for testing). |

Common to modes 2 and 3: `WIN_CODESIGN_PASS` (PIN/passphrase),
`WIN_CODESIGN_TS` (RFC-3161 timestamp URL, default `http://timestamp.digicert.com`).
Modes 2 and 3 require `osslsigncode` installed (`apt install osslsigncode`).

> **Which cert clears SmartScreen?** An **EV** cert clears it *immediately*. An **OV**
> or individual/open-source cert only clears it once the signed binary has earned
> enough **download reputation** (days–weeks of installs). A **self-signed** cert never
> clears it for other people — it's only useful on machines where you've trusted your
> own root.

---

## Getting a certificate

**For bsdrX, buy a commercial cert and sign locally.** bsdrX is a commercial product
(closed-source paid plugins, binaries distributed from our own website, builds done
locally — **not** in cloud CI). That rules out the free open-source programs:
**SignPath Foundation does not fit** — it signs server-side from CI, forbids proprietary
components published by the maintainer, and verifies the binary was built from the
public repo. A *purchased* cert has no such constraints and signs on the local build
machine. Recommended: **Azure Trusted Signing** (cheapest, no token) or a bought **EV
cert on a token** (instant SmartScreen, no cloud). Vendor still TBD — both are wired.

### Recommended — Azure Trusted Signing (cheap, no token, local build)

<https://learn.microsoft.com/azure/trusted-signing/> — Microsoft-run, ~**$9.99/month**,
gains SmartScreen reputation quickly (now open to individuals, subject to an identity
check). Signing is done by their `.NET` `Azure.CodeSigning` dlib tool / `dotnet sign`,
in place — **mode 1**:

```sh
export WIN_CODESIGN_CMD='dotnet sign code trusted-signing {} \
  --trusted-signing-account $ACCT --certificate-profile $PROFILE \
  --endpoint https://eus.codesigning.azure.net/'
WIN_DEPS=/path/to/win-deps bash scripts/build-win-bundle.sh
```

### Cloud eSigner (SSL.com) — EV, no USB token

SSL.com **eSigner** signs through their cloud HSM (no physical token). Their
`CodeSignTool` signs in place — **mode 1**:

```sh
export WIN_CODESIGN_CMD='CodeSignTool sign -username=$SSL_USER -password=$SSL_PASS \
  -totp_secret=$SSL_TOTP -credential_id=$SSL_CRED -input_file_path={}'
WIN_DEPS=/path/to/win-deps bash scripts/build-win-bundle.sh
```

### Hardware token / cloud HSM (SafeNet, YubiKey, Azure Key Vault) — mode 2

Since June 2023 most OV/EV certs ship on a **FIPS-140 token** or must live in an HSM.
Drive it through osslsigncode's PKCS#11 engine:

```sh
export WIN_CODESIGN_PKCS11_MODULE=/usr/lib/x86_64-linux-gnu/pkcs11/opensc-pkcs11.so
export WIN_CODESIGN_KEY='pkcs11:object=my-key;type=private'   # key URI or label
export WIN_CODESIGN_CERT=/path/to/cert.pem                     # or WIN_CODESIGN_PKCS11_CERT=<id>
export WIN_CODESIGN_PASS=123456                                # token PIN
# export WIN_CODESIGN_PKCS11_ENGINE=/usr/lib/.../pkcs11.so     # only if not the default engine
WIN_DEPS=/path/to/win-deps bash scripts/build-win-bundle.sh
```

### Local .pfx — testing only (mode 3)

A self-signed cert does **not** clear SmartScreen for other people, but it verifies the
signing pipeline and is fine on machines where you trust your own root.

```sh
# make a throwaway self-signed code-signing cert:
openssl req -x509 -newkey rsa:2048 -keyout k.pem -out c.pem -days 365 -nodes \
  -subj "/CN=bsdrX test" -addext "extendedKeyUsage=codeSigning"
openssl pkcs12 -export -in c.pem -inkey k.pem -out test.pfx -passout pass:test

export WIN_CODESIGN_PFX=$PWD/test.pfx
export WIN_CODESIGN_PASS=test
WIN_DEPS=/path/to/win-deps bash scripts/build-win-bundle.sh
```

Verify the result on the signed exe:

```sh
osslsigncode verify build-windows-media/bsdr_agent.exe
```

---

## What signing does *not* fix

- **Npcap / VB-CABLE prompts** are separate — those are the optional external installers
  the NSIS installer offers (`SecNpcap`, `SecVBCable`); the agent runs without them.
- Signing does not change behavior, only the publisher identity Windows shows.
