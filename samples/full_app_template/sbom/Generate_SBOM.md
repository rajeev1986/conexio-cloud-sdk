# Generating an SBOM for the Conexio Cloud SDK

This guide covers generating a Software Bill of Materials (SBOM) for the
`full_app_template` sample (or any other Conexio Cloud SDK sample) targeting
the **Conexio Stratus Pro / nRF9151** board under NCS v3.4.1.

The output is a **CycloneDX 1.7 JSON** file compatible with OSV.dev, Black Duck,
NTIA compliance checkers, and other SBOM tooling.

---

## 1. Toolchain Requirements

### 1.1 NCS Toolchain

Install **nRF Connect SDK v3.4.1** and its bundled toolchain using the
[nRF Connect for Desktop](https://www.nordicsemi.com/Products/Development-tools/nRF-Connect-for-Desktop)
Toolchain Manager, or follow the
[NCS Getting Started guide](https://docs.nordicsemi.com/bundle/ncs-3.4.1/page/nrf/installation/install_ncs.html).

After installation the NCS toolchain lives at:
```
/opt/nordic/ncs/toolchains/<bundle_id>/
```

### 1.2 Python Dependencies

The `west ncs-sbom` command requires two Python packages. Install them into
the NCS toolchain Python:

```bash
NCS_PYTHON=/opt/nordic/ncs/toolchains/97226bad62/opt/python@3.12/bin/python3.12

# SPDX tools (usually pre-installed with NCS 3.4.1)
$NCS_PYTHON -m pip install spdx-tools

# scancode-toolkit — performs licence detection on every source file
$NCS_PYTHON -m pip install scancode-toolkit

# scancode native plugins (already bundled in NCS toolchain)
$NCS_PYTHON -m pip install extractcode-libarchive typecode-libmagic
```

> **Note:** `scancode-toolkit` is large (~300 MB) and takes a few minutes to
> install. It is optional but strongly recommended — without it most files will
> show `NOASSERTION` for their licence.

#### Expected warnings on macOS

When running `west ncs-sbom` on macOS you will see these warnings — they are
**cosmetic only** and do not affect the SBOM output:

```
UserWarning: Using "libarchive" library found in a system location.
  Install instead a extractcode-libarchive plugin for best support.

UserWarning: System libmagic found in typical location is used.
  Install instead a typecode-libmagic plugin for best support.

UserWarning: Libmagic magic database not found. A default will be used
  if possible. OR set the TYPECODE_LIBMAGIC_DB_PATH environment variable.
```

**Root cause:** On macOS the pre-compiled native libraries bundled inside the
`extractcode-libarchive` and `typecode-libmagic` plugins are built for Linux.
scancode correctly falls back to the Homebrew-installed system `libarchive` and
`libmagic`, which work identically.

**To suppress the magic database warning**, set `TYPECODE_LIBMAGIC_DB_PATH`
to point to the Homebrew magic database before running `west ncs-sbom`:

```bash
export TYPECODE_LIBMAGIC_DB_PATH=/opt/homebrew/Cellar/libmagic/5.47/share/misc/magic.mgc
```

The `libarchive` and `libmagic system location` warnings cannot be suppressed
on macOS without recompiling the plugins, but they have no impact on accuracy.

### 1.3 syft (CycloneDX conversion)

`syft` converts the SPDX tag-value output of `west ncs-sbom` into the
**CycloneDX JSON** format required by OSV.dev and other CVE scanners.

**macOS (Homebrew):**
```bash
brew install syft
```

**macOS / Linux (official install script):**
```bash
curl -sSfL https://raw.githubusercontent.com/anchore/syft/main/install.sh \
  | sh -s -- -b /usr/local/bin
syft version
```

---

## 2. Generating the SBOM

The NCS SBOM flow has three stages: **init → build → generate**.

### 2.1 Set up environment variables

```bash
export PATH="/opt/nordic/ncs/toolchains/97226bad62/Cellar/python@3.12/3.12.4/\
Frameworks/Python.framework/Versions/3.12/bin:\
/opt/nordic/ncs/toolchains/97226bad62/bin:$PATH"

export ZEPHYR_SDK_INSTALL_DIR=/opt/nordic/ncs/toolchains/97226bad62/opt/zephyr-sdk

# Suppress the "Libmagic magic database not found" warning on macOS
export TYPECODE_LIBMAGIC_DB_PATH=/opt/homebrew/Cellar/libmagic/5.47/share/misc/magic.mgc

# All west commands must run from the NCS workspace root
cd /opt/nordic/ncs/v3.4.1
```

### 2.2 Define paths

```bash
SAMPLE=/opt/nordic/ncs/v3.4.1/modules/lib/conexio-cloud-sdk/samples/full_app_template
BUILD_DIR=$SAMPLE/build
SBOM_DIR=$SAMPLE/sbom
BOARD=conexio_stratus_pro/nrf9151/ns
BOARD_ROOT=/opt/nordic/ncs/v3.4.1/conexio-firmware-sdk
```

### 2.3 Step 1 — Initialise the CMake file-based API

This creates a query file that instructs CMake to emit the codemodel data
that `west ncs-sbom` needs to map object files back to source files.

```bash
west spdx --init -d "$BUILD_DIR"
```

> This must be done **before** the build. If you already have a build directory,
> delete it first (`rm -rf "$BUILD_DIR"`) and re-run this step.

### 2.4 Step 2 — Build the application

```bash
west build \
  --build-dir "$BUILD_DIR" \
  "$SAMPLE" \
  --pristine \
  --board "$BOARD" \
  -- -DBOARD_ROOT="$BOARD_ROOT"
```

### 2.5 Step 3 — Generate the SBOM

```bash
mkdir -p "$SBOM_DIR"

west ncs-sbom \
  -d "$BUILD_DIR" \
  --output-html "$SBOM_DIR/full_app_template_sbom.html" \
  --output-spdx "$SBOM_DIR/full_app_template_sbom.spdx" \
  --package-supplier "Conexio Technologies, Inc" \
  --scancode /opt/nordic/ncs/toolchains/97226bad62/bin/scancode
```

| Flag | Description |
|------|-------------|
| `-d` | Build directory produced in Step 2 |
| `--output-html` | Human-readable licence report |
| `--output-spdx` | Machine-readable SPDX 2.2 tag-value document |
| `--package-supplier` | Supplier name embedded in the SBOM |
| `--scancode` | Path to the `scancode` binary for licence detection |

> **Runtime:** expect 10–20 minutes. `scancode-toolkit` scans every source
> file in the Zephyr + NCS tree (~1 000+ files) in parallel.

This produces two files per image (application + MCUboot):
```
sbom/full_app_template_sbom_full_app_template.html
sbom/full_app_template_sbom_full_app_template.spdx
sbom/full_app_template_sbom_mcuboot.html
sbom/full_app_template_sbom_mcuboot.spdx
```

---

## 3. Converting to CycloneDX JSON

OSV.dev (and most CVE scanners) require either **CycloneDX JSON** or
**SPDX 2.3 JSON**. The `west ncs-sbom` tool outputs SPDX 2.2 tag-value, so
a conversion step is needed.

Use `syft convert` to produce a CycloneDX 1.7 JSON file:

```bash
# Application image
syft convert \
  "$SBOM_DIR/full_app_template_sbom_full_app_template.spdx" \
  -o cyclonedx-json="$SBOM_DIR/full_app_template_sbom.cdx.json"

# MCUboot image (optional — include for a complete system SBOM)
syft convert \
  "$SBOM_DIR/full_app_template_sbom_mcuboot.spdx" \
  -o cyclonedx-json="$SBOM_DIR/mcuboot_sbom.cdx.json"
```

> `syft convert` is marked experimental but works reliably for SPDX → CycloneDX
> conversion. The `WARN convert is an experimental feature` message is expected
> and can be ignored.

### 3.1 Verify the output

```bash
python3 -c "
import json, sys
with open('$SBOM_DIR/full_app_template_sbom.cdx.json') as f:
    d = json.load(f)
print('Format   :', d['bomFormat'])
print('SpecVer  :', d['specVersion'])
print('Components:', len(d.get('components', [])))
"
```

Expected output:
```
Format   : CycloneDX
SpecVer  : 1.7
Components: 1187
```

### 3.2 Upload to OSV.dev

1. Go to [https://osv.dev](https://osv.dev)
2. Click **"Scan your project"**
3. Upload **`full_app_template_sbom.cdx.json`**
4. OSV.dev will match each component against its CVE database and report
   any known vulnerabilities

---

## 4. Output File Reference

| File | Format | Purpose |
|------|--------|---------|
| `full_app_template_sbom_full_app_template.html` | HTML | Human-readable licence report for the application |
| `full_app_template_sbom_full_app_template.spdx` | SPDX 2.2 tag-value | Machine-readable SBOM (application) |
| `full_app_template_sbom_mcuboot.html` | HTML | Human-readable licence report for MCUboot |
| `full_app_template_sbom_mcuboot.spdx` | SPDX 2.2 tag-value | Machine-readable SBOM (MCUboot) |
| `full_app_template_sbom.cdx.json` | **CycloneDX 1.7 JSON** | Upload to OSV.dev / CVE scanners |
| `mcuboot_sbom.cdx.json` | **CycloneDX 1.7 JSON** | MCUboot CVE scan (optional) |

---

## 5. Adapting for Other Samples

Replace the `SAMPLE` path and adjust `--output-html` / `--output-spdx` names:

```bash
SAMPLE=/opt/nordic/ncs/v3.4.1/modules/lib/conexio-cloud-sdk/samples/hello
BUILD_DIR=$SAMPLE/build
SBOM_DIR=$SAMPLE/sbom
```

Then repeat Steps 2.3 → 2.5 → 3.

---

## 6. References

- [NCS SBOM documentation](https://docs.nordicsemi.com/bundle/ncs-3.4.1/page/nrf/scripts/west_commands/sbom/README.html)
- [Zephyr `west spdx` documentation](https://docs.zephyrproject.org/latest/develop/west/build-flash-debug.html#west-spdx)
- [OSV.dev SBOM scanning](https://osv.dev)
- [syft documentation](https://github.com/anchore/syft)
- [CycloneDX specification](https://cyclonedx.org/specification/overview/)
- [SPDX specification](https://spdx.github.io/spdx-spec/v2.3/)

---

## 7. Alternative: Using `west spdx` (Zephyr built-in, no extra dependencies)

`west spdx` is the SPDX generator that ships with Zephyr itself. It requires
**no external tools** beyond the standard NCS toolchain — no scancode-toolkit,
no syft. It is faster than `west ncs-sbom` (seconds vs minutes) but produces
a leaner SPDX document: source-file provenance and package relationships are
included, but per-file licence detection is skipped.

Use this method when:
- You need a quick SBOM for compliance or CVE scanning
- You do not have scancode-toolkit installed
- You want to upload to the Conexio Cloud Security scanner

---

### 7.1 Set up the environment

```bash
export PATH="/opt/nordic/ncs/toolchains/97226bad62/Cellar/python@3.12/3.12.4/\
Frameworks/Python.framework/Versions/3.12/bin:\
/opt/nordic/ncs/toolchains/97226bad62/bin:$PATH"

export ZEPHYR_SDK_INSTALL_DIR=/opt/nordic/ncs/toolchains/97226bad62/opt/zephyr-sdk
export ZEPHYR_BASE=/opt/nordic/ncs/v3.4.1/zephyr
export BOARD_ROOT=/opt/nordic/ncs/v3.4.1/conexio-firmware-sdk

WEST=/opt/nordic/ncs/toolchains/97226bad62/Cellar/python@3.12/3.12.4/\
Frameworks/Python.framework/Versions/3.12/bin/west

SAMPLE=/opt/nordic/ncs/v3.4.1/modules/lib/conexio-cloud-sdk/samples/full_app_template
BUILD_DIR=$SAMPLE/build
SBOM_DIR=$SAMPLE/sbom
BOARD=conexio_stratus_pro/nrf9151/ns

cd /opt/nordic/ncs/v3.4.1
```

---

### 7.2 Step 1 — Initialise the CMake file-based API

This creates a query stub that instructs CMake to emit codemodel data.
**Must be run before the build** — if a build directory already exists,
delete it first (`rm -rf "$BUILD_DIR"`).

```bash
$WEST spdx --init -d "$BUILD_DIR"
```

You should see output similar to:

```
Writing cmake file API query file to: <build_dir>/CMakeFiles/FileAPI/query/v1/codemodel
```

---

### 7.3 Step 2 — Build the application

```bash
$WEST build \
  --build-dir "$BUILD_DIR" \
  "$SAMPLE" \
  --pristine \
  --board "$BOARD" \
  -- -DBOARD_ROOT="$BOARD_ROOT"
```

The build now populates the CMake file-based API reply files that `west spdx`
will consume in the next step.

---

### 7.4 Step 3 — Generate the SPDX document

```bash
mkdir -p "$SBOM_DIR"

$WEST spdx \
  -d "$BUILD_DIR" \
  --spdx-output "$SBOM_DIR/full_app_template_sbom_full_app_template.spdx"
```

| Flag | Description |
|------|-------------|
| `-d` | Build directory from Step 2 |
| `--spdx-output` | Output `.spdx` file path |

This takes **5–30 seconds** and produces:

```
sbom/full_app_template_sbom_full_app_template.spdx
```

---

### 7.5 Verify the output

```bash
head -5 "$SBOM_DIR/full_app_template_sbom_full_app_template.spdx"
grep -c "^PackageName:" "$SBOM_DIR/full_app_template_sbom_full_app_template.spdx"
grep "^PackageVersion:" "$SBOM_DIR/full_app_template_sbom_full_app_template.spdx" \
  | grep -v NOASSERTION | head -5
```

Expected output:

```
# Document Information
SPDXVersion: SPDX-2.2
DataLicense: CC0-1.0
...
13                        ← packages found
nrfconnect/sdk-cjson @ c6af068b...
nrfconnect/sdk-nrf @ ...
```

> **Note:** Most packages will have `PackageVersion: NOASSERTION` — this is
> normal for Zephyr source trees where components are git submodules rather
> than versioned package releases. Packages that are tracked via west manifests
> (e.g. `sdk-nrf`, `sdk-zephyr`, `sdk-mbedtls`) will carry their git commit
> hash as the version and a `pkg:github/` PURL in their `ExternalRef` line.
> These PURLs are what OSV.dev uses for CVE matching.

---

### 7.6 Upload to Conexio Cloud Security scanner

The `.spdx` tag-value file can be uploaded directly to the Conexio Cloud
dashboard at **Firmware Update → Security → Upload SBOM**.

The scanner supports SPDX 2.x tag-value files including those generated by
`west spdx` and `west ncs-sbom`. It extracts packages that have either a
valid `PackageVersion` (non-NOASSERTION) or a `pkg:` PURL in their
`ExternalRef: PACKAGE-MANAGER purl` line, then queries OSV.dev for CVEs.

---

### 7.7 Convert to CycloneDX JSON (optional)

If you also need a CycloneDX JSON (for OSV.dev direct upload or other tools),
convert using `syft` as described in **Section 3** of this guide.

---

### 7.8 Comparison: `west spdx` vs `west ncs-sbom`

| Feature | `west spdx` | `west ncs-sbom` |
|---------|-------------|-----------------|
| **Speed** | 5–30 seconds | 10–20 minutes |
| **Extra dependencies** | None | scancode-toolkit |
| **Licence detection** | ✗ (NOASSERTION) | ✓ (per-file SPDX expressions) |
| **Package count** | ~13 (west modules) | Same |
| **CVE scanning** | ✓ (via PURLs) | ✓ (via PURLs) |
| **Conexio Cloud upload** | ✓ | ✓ |
| **OSV.dev upload** | Convert to CycloneDX first | Convert to CycloneDX first |

For CVE scanning purposes both methods produce equivalent results — the key
information is the set of packages and their PURLs, which both tools emit.
Use `west spdx` for speed; use `west ncs-sbom` when you also need the
per-file licence report for compliance documentation.
