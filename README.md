# Address Cleaner

A Windows desktop tool for cleaning real estate property owner names for mail merge and address labels. Paste rows from Excel, click Clean, paste back.

---

## What it does

Raw property records contain trust language, legal boilerplate, and formatting that makes poor mailing labels. Address Cleaner strips it:

| Raw Input | Cleaned Output |
|-----------|----------------|
| `JOHN & MARY SMITH REVOCABLE LIVING TRUST` | `John & Mary Smith` |
| `MAXWELL FAMILY LIVING TRUST` | `Maxwell Family` |
| `C/O ESTATE OF JAMES WILSON DECEASED` | `James Wilson` |
| `JOHNSON MARY K` | `Mary K Johnson` *(AI)* |
| `KJ POLLAK FAMILY TRUST` | `KJ Pollak Family` *(AI)* |
| `NOT AVAILABLE FROM THE DATA SOURCE` | `Current Resident` |

---

## GUI

```
┌────────────────────────────────────────────────────────────────┐
│ [logo if present]              [Clear] [Copy] [Clean»]         │
├────────────────────────────┬───────────────────────────────────┤
│ Paste Excel rows here      │ Cleaned output (read-only)        │
│                            │                                   │
│ JOHN SMITH REVOCABLE...    │ John Smith                        │
│ C/O JANE DOE               │ Jane Doe                          │
│ KJ POLLAK FAMILY TRUST     │ KJ Pollak Family                  │
│                            │                                   │
├────────────────────────────┴───────────────────────────────────┤
│ 3 rows cleaned | 2 trusts stripped          AI: Ready          │
└────────────────────────────────────────────────────────────────┘
```

**Workflow:**
1. Copy rows from Excel (Ctrl+C)
2. Paste into the left pane (Ctrl+V)
3. Click **Clean** — output appears on the right
4. Click **Copy** — paste back into Excel (Ctrl+V)

The input can be any column count. Only the first (name) column is modified; all other columns pass through unchanged.

---

## Logo (optional)

Place a PNG file at `config\logo.png` next to `nameclean.exe` to display a custom logo:

- Shown in the **splash screen** (top center, composited on white background)
- Shown in the **main window toolbar** (left side, composited on the system button-face color — no white-box artifact)
- Recommended size: **621 × 100 px** — any size is accepted and scaled to fit
- Loaded once at startup via GDI+ (no runtime DLL dependency — loaded dynamically)
- If the file is absent, the app runs normally with no logo

```
nameclean\
  nameclean.exe
  config\
    logo.png    ← optional; any PNG, recommended 621×100 px
```

---

## Building

Requires [MinGW-w64](https://www.mingw-w64.org/) (GCC on PATH).

```bash
mingw32-make              # release build → nameclean.exe
mingw32-make DEBUG=1      # debug build (console window, symbols, log file)
mingw32-make clean        # remove .o files and exe
```

**Dependencies:** All linked libraries (`comctl32`, `gdi32`, `winhttp`) are standard Windows system DLLs. No external packages needed.

---

## AI sidecar (optional)

The core rules engine handles ~95% of names. For edge cases — reversed names, typos, ambiguous initials — an optional local AI model provides a second pass.

**Quick setup — automatic (recommended):**

On first launch, if the AI components are missing, the app offers to download them automatically. Click **Yes** and it handles everything: detects your GPU, downloads the right [llama.cpp](https://github.com/ggml-org/llama.cpp) build, extracts it, and downloads the [Qwen2.5-3B-Instruct Q4_K_M](https://huggingface.co/Qwen/Qwen2.5-3B-Instruct-GGUF) model (~2 GB). Click **No** to continue in rules-only mode.

**Alternative — manual script (double-click):**
```
setup.bat
```

The script does the same download, placing everything in an `ai\` subdirectory next to `nameclean.exe`. Safe to re-run — skips components already present. To remove AI, delete the `ai\` folder.

> **Restricted environments:** All temporary files (downloads, scripts, extraction) are stored in a local `tmp\` directory next to the executable — not in `%TEMP%` or AppData. This works in corporate environments where roaming/local AppData is locked down. The `tmp\` directory is cleaned up automatically after setup completes.

**Startup:** On first launch after AI is installed, a splash screen appears while the model loads (60–90 seconds typical). The main window opens automatically when the model is ready. If startup times out or fails, the app falls back to rules-only mode.

**Status bar shows:**
- `AI: Loading...` — server starting; splash screen is visible
- `AI: Ready` — AI is active
- `AI: No model` — sidecar files not found; rules engine only

See [DEPLOY.md](DEPLOY.md) for manual deployment, CUDA vs CPU builds, and DLL details.

---

## Cleaning rules

Rules run in priority order on an uppercase working copy:

1. **C/O prefix** — `C/O JOHN SMITH` → `JOHN SMITH`
2. **Date clauses** — truncates at `DATED`, `DTD`, `U/D/T`, `UTD`, `AMENDED`, `RESTATED`
3. **Trust type suffixes** — strips longest match first:
   - `REVOCABLE LIVING TRUST`, `IRREVOCABLE INTER VIVOS TRUST`, `FAMILY TRUST` (keeps FAMILY), `TRUST`, and ~40 other variants
   - Includes `LEGACY TRUST`, `HERITAGE TRUST`, `SURVIVORS TRUST`, `EXEMPT TRUST`, `SUPPLEMENTAL NEEDS TRUST`, and estate acronyms (`GRAT`, `GRUT`, `CRUT`, `CRAT`, `QPRT`, `ILIT`)
   - Uses `str_erase` to preserve content after trust patterns (e.g., co-owner names)
   - Handles truncated/garbled forms: `TRUS`, `TRU`, `SURVIVIORS`, etc.
4. **Trustee language** — `AS TRUSTEE`, `SUCCESSOR TRUSTEE`, `AS CO-TRUSTEE`
5. **Leading THE** — removed unless FAMILY is in the name
6. **Junk words** — `ET AL`, `ET UX`, `DECEASED`, `ESTATE OF`, `UNKNOWN`, etc.
7. **Embedded addresses** — strips trailing street addresses

**Title case exceptions:**

| Type | Examples |
|------|---------|
| Always uppercase | `LLC`, `LLP`, `LP`, `INC`, `CORP`, `LTD`, `PC`, `NA`, `FSB`, `PLLC`, `USA` |
| Always lowercase (non-initial) | `of`, `and`, `or`, `the`, `for`, `in`, `on`, `at`, `to`, `by` |
| Exact form | `Mr.`, `Mrs.`, `Dr.`, `Jr.`, `Sr.`, `II`, `III`, `IV` |
| Smart prefixes | `McCoy`, `MacDonald` (6+ chars only), `O'Brien`, hyphenated segments |

---

## Project structure

```
Address Cleaner/
├── src/
│   ├── main.c      — WinMain, window creation, LLM startup, splash trigger
│   ├── gui.c/h     — Win32 controls, layout, button handlers, logo rendering
│   ├── splash.c/h  — AI startup splash screen, PNG logo loader (GDI+)
│   ├── tsv.c/h     — Tab-separated value parser (Excel paste format)
│   ├── names.c/h   — Name pipeline: rules → title case → AI fallback
│   ├── rules.c/h   — Pattern rules engine, flag bitmask definitions
│   ├── llm.c/h     — llama-server sidecar: process management, HTTP, prompt
│   ├── setup.c/h   — In-app AI dependency downloader (first-run wizard)
│   └── slog.c/h    — Session logging (logs\ directory, TSV per Clean click)
├── Makefile
├── setup.bat       — Alternative AI dependency downloader (script)
├── DEPLOY.md       — Distribution and DLL reference
└── README.md

[runtime, not in repo]
├── config\         — Optional configuration files
│   └── logo.png    — Custom logo (621×100 px recommended; any PNG accepted)
├── ai\             — AI sidecar files (created by setup wizard or setup.bat)
│   ├── llama-server.exe
│   ├── ggml.dll / llama.dll
│   └── qwen2.5-3b-instruct-q4_k_m.gguf
├── tmp\            — Temporary files during setup (auto-cleaned after completion)
└── logs\           — Session logs (created automatically on first Clean)
    └── session_YYYYMMDD_HHMMSS.tsv
```

---

## Known limitations

- **Reversed Last-First names** (`SIRAKIS DEREK M` → `Derek M Sirakis`) are detected and reordered automatically; AI then checks for typos in the corrected form.
- **Short entity abbreviations** (≤3 chars without a vowel, e.g., `HEB`, `KJ`) are sent to AI; without AI they title-case incorrectly (e.g., `Heb`). 4+ char single-word results from family trusts get "Family" appended (e.g., `RUIZ FAMILY TRUST` → `Ruiz Family`); other trust types keep "Trust" (e.g., `MAXWELL LIVING TRUST` → `Maxwell Trust`).
- **Garbled truncated first names** (`LNDY`) are left as-is rather than guessed.

---

## Requirements

- Windows 10 or 11 (64-bit)
- MinGW-w64 GCC to build from source
- AI features: ~2.5 GB disk space; runs CPU-only (no GPU required)
