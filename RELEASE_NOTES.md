# Release Notes

## v0.2.0 — Name Cleaning Overhaul (2026-03-09)

### Highlights

- Replaced 170+ hardcoded truncation patterns with a vocabulary-based algorithm that handles any combination of trust words at any truncation point
- Added 20+ new trust type patterns (legacy trusts, heritage trusts, survivor trusts, estate acronyms)
- Fixed buffer safety issues and achieved strict C99 compliance
- Verified zero regressions across 500-row production dataset

---

### New Features

**Vocabulary-based truncation stripping** — `strip_truncated_suffix()` now uses a two-tier vocabulary table with four processing phases instead of an exhaustive pattern list:

- Phase 1: Right-to-left iterative stripping with tier-1 anchors and tier-2 context words
- Phase 2: Forward anchor scan to catch short trailing fragments (e.g., "REVOCABLE T")
- Phase 3: Fuzzy SURVIVOR matching (catches garbled forms like SURVIIVO, SURVIVIOS)
- Phase 4: Standalone LIVING at end of 3+ word names

**Expanded trust type coverage:**

- Legacy/Heritage trusts: `LEGACY LIVING TRUST`, `HERITAGE TRUST`, etc.
- Survivor trusts: `SURVIVORS TRUST`, `SURVIVOR'S TRUST`
- Other types: `EXEMPT TRUST`, `SUPPLEMENTAL NEEDS TRUST`
- Estate acronyms: `GRAT`, `GRUT`, `CRUT`, `CRAT`, `QPRT`, `ILIT`
- Amendment forms: `AMENDED AND RESTATED TRUST`, `RESTATEMENT OF TRUST`

**New date keywords:** `AS AMENDED`, `AMENDED AND RESTATED`, `AMENDED`, `RESTATED`

---

### Bug Fixes

- **Mac prefix false positives** — Raised minimum word length from 4 to 6 characters, preventing `MACON` from becoming `MacOn` while still correctly handling `MacNeil`, `MacDonald`, etc.
- **Single-word threshold** — Changed from ≤4 to ≤3 characters. Names like `RUIZ` (4 chars) now get "Family" appended consistently with longer surnames, instead of being flagged for AI review.
- **Buffer overflow in `collapse_spaced_acronym`** — Added bounds checks on all write paths for both `run[]` and `out[]` buffers.
- **Unsafe string copy in `name_clean`** — Replaced unbounded `memcpy` with length-checked copy and explicit null termination.
- **Trust suffix stripping data loss** — Rewrote `strip_trust_suffix()` to use `str_erase` instead of null-termination, preserving content that follows trust patterns (e.g., co-owner names after `& `). Loops to handle multiple trust patterns in a single name.
- **Trust keyword divergence** — The quick-strip arrays in `strip_and_trust()` and `extract_cotenant()` were missing 20+ trust types. Hoisted to a shared file-scope `trust_keywords_quick[]` array with full coverage.

---

### Code Quality

- **C99 compliance** — Moved `TrustVocab` typedef, `trunc_vocab[]` table, and `is_trunc_vocab_prefix()` from GCC nested function to proper file-scope declarations. Compiles cleanly on GCC, MSVC, and Clang.
- **Zero warnings** — Clean build with `-Wall -Wextra -std=c99 -O2`.
- **Deduplicated trust keyword tables** — Eliminated duplicate `tkw[]` arrays; all trust keyword lists are now maintained in one place.

---

### Stats

- 3 files changed, +446 / -277 lines
- 3 commits on `dev/name-cleaning-improvements`, fast-forward merged to `master`
- 500-row production dataset: zero regressions

---

## v0.1.0-alpha — Initial Release

- Win32 GUI with paste-in / clean / copy-out workflow
- Rules-based name cleaning pipeline (C/O, dates, trust suffixes, trustees, junk words)
- Title case with smart prefix handling (Mc/Mac/O'/hyphen)
- Optional AI sidecar via llama.cpp (Qwen2.5-3B)
- Splash screen with PNG logo support
- Session logging
