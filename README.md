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
│  [Append notes □]                      [Clear] [Copy] [Clean»] │
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

## Building

Requires [MinGW-w64](https://www.mingw-w64.org/) (GCC on PATH).

```bash
make              # release build → nameclean.exe
make DEBUG=1      # debug build (console window, symbols, log file)
make clean        # remove .o files and exe
```

**Dependencies:** All linked libraries (`comctl32`, `gdi32`, `winhttp`) are standard Windows system DLLs. No external packages needed.

---

## AI sidecar (optional)

The core rules engine handles ~95% of names. For edge cases — reversed names, typos, ambiguous initials — an optional local AI model provides a second pass.

**Quick setup (double-click):**
```
setup.bat
```

The script detects your GPU, downloads the correct [llama.cpp](https://github.com/ggml-org/llama.cpp) release and the [Qwen2.5-1.5B](https://huggingface.co/Qwen/Qwen2.5-1.5B-Instruct-GGUF) model (~940 MB), and places everything in an `ai\` subdirectory next to `nameclean.exe`. Safe to re-run — skips if already set up. To remove AI, just delete the `ai\` folder.

**Status bar shows:**
- `AI: Loading...` — server starting (up to ~120 sec on first run)
- `AI: Ready` — AI is active
- `AI: No model` — sidecar files not found; rules engine only

See [DEPLOY.md](DEPLOY.md) for manual deployment, CUDA vs CPU builds, and DLL details.

---

## Cleaning rules

Rules run in priority order on an uppercase working copy:

1. **C/O prefix** — `C/O JOHN SMITH` → `JOHN SMITH`
2. **Date clauses** — truncates at `DATED`, `DTD`, `U/D/T`, `UTD`
3. **Trust type suffixes** — strips longest match first:
   - `REVOCABLE LIVING TRUST`, `IRREVOCABLE INTER VIVOS TRUST`, `FAMILY TRUST` (keeps FAMILY), `TRUST`, and ~30 other variants
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
| Smart prefixes | `McCoy`, `MacDonald`, `O'Brien`, hyphenated segments |

---

## Project structure

```
Address Cleaner/
├── src/
│   ├── main.c      — WinMain, window creation, LLM startup
│   ├── gui.c/h     — Win32 controls, layout, button handlers
│   ├── tsv.c/h     — Tab-separated value parser (Excel paste format)
│   ├── names.c/h   — Name pipeline: rules → title case → AI fallback
│   ├── rules.c/h   — Pattern rules engine, flag bitmask definitions
│   ├── llm.c/h     — llama-server sidecar: process management, HTTP, prompt
│   └── slog.c/h    — Session logging (logs\ directory, TSV per Clean click)
├── Makefile
├── setup.bat       — First-run AI dependency downloader
├── DEPLOY.md       — Distribution and DLL reference
└── README.md

[runtime, not in repo]
├── ai\             — AI sidecar files (created by setup.bat)
│   ├── llama-server.exe
│   ├── ggml.dll / llama.dll
│   └── *.gguf
└── logs\           — Session logs (created automatically on first Clean)
    └── session_YYYYMMDD_HHMMSS.tsv
```

---

## Known limitations

- **Reversed Last-First names** (`SIRAKIS DEREK M` → `Derek M Sirakis`) are detected and reordered automatically; AI then checks for typos in the corrected form.
- **Short entity abbreviations** without a vowel (`HEB`, `KJ`) are sent to AI; without AI they title-case incorrectly (e.g., `Heb`).
- **Garbled truncated first names** (`LNDY`) are left as-is rather than guessed.

---

## Requirements

- Windows 10 or 11 (64-bit)
- MinGW-w64 GCC to build from source
- AI features: ~2 GB disk space; NVIDIA GPU optional (CPU works fine)
