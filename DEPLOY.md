# Address Cleaner — Distribution & DLL Dependencies

## nameclean.exe — DLL requirements

Verified with `objdump -p nameclean.exe` (UCRT64 build):

| DLL | Source | Notes |
|-----|--------|-------|
| `KERNEL32.dll` | Windows built-in | Always present |
| `USER32.dll` | Windows built-in | Always present |
| `GDI32.dll` | Windows built-in | Always present |
| `COMCTL32.dll` | Windows built-in | Always present (status bar, common controls) |
| `WINHTTP.dll` | Windows built-in | Always present (Win XP SP3+) |
| `api-ms-win-crt-*.dll` (7 DLLs) | Windows UCRT | See note below |

**UCRT note:** The `api-ms-win-crt-*` forwarding DLLs are part of the Universal C Runtime.
- **Windows 10 / 11:** always present — no action needed.
- **Windows 7 SP1 / 8.1:** install [KB2999226](https://support.microsoft.com/kb/2999226)
  (or the Visual C++ 2015–2022 Redistributable, which includes UCRT).

No MinGW runtime DLLs (`libgcc_s_seh-1.dll`, `libwinpthread-1.dll`) are needed —
the UCRT64 toolchain links the C++ support library statically by default.

**Result: `nameclean.exe` requires zero extra files on any Windows 10/11 machine.**

---

## AI sidecar — optional, two deployment options

The AI feature is entirely optional. Without it the rules engine still handles ~95% of
names correctly. With it, edge cases (reversed Last-First, typos, mangled words) are
automatically fixed.

Both the **in-app setup wizard** (first-run prompt) and **`setup.bat`** detect your GPU automatically and download the appropriate build — CUDA for NVIDIA, Vulkan for AMD, CPU otherwise. To set up manually, choose the variant that matches your hardware.

> **Restricted environments:** Setup stores all temporary files (downloaded zips, PowerShell scripts, extraction output) in a local `tmp\` directory next to the executable — never in `%TEMP%`, `%APPDATA%`, or `%LOCALAPPDATA%`. This avoids issues in corporate environments where AppData or the system temp folder is restricted. The `tmp\` directory is automatically removed after setup completes.

> **Note:** `nameclean.exe` always launches llama-server with `--n-gpu-layers 0` (CPU
> inference), regardless of which build is installed. Even with a CUDA or Vulkan build,
> computation runs on the CPU. To enable GPU offload, edit the flag in `llm.c` and rebuild.

### Option A — CPU build (no GPU required)

Download the **CPU** release of llama.cpp:
<https://github.com/ggml-org/llama.cpp/releases>
File: `llama-<version>-bin-win-cpu-x64.zip`

Files to place in **`ai\` next to `nameclean.exe`**:

| File | Required |
|------|----------|
| `llama-server.exe` | Yes |
| `ggml.dll` | Yes (if not statically linked in the release) |
| `llama.dll` | Yes (if not statically linked in the release) |
| `qwen2.5-3b-instruct-q4_k_m.gguf` | Yes (any `*.gguf` filename is accepted) |

> Check the release zip — some llama.cpp releases ship `llama-server.exe` as a single
> statically-linked binary with no DLL dependencies at all.

**No CUDA, no driver requirements, runs on any modern CPU.**

---

### Option B — CUDA / GPU-accelerated build

> GPU offload requires editing `--n-gpu-layers` in `llm.c` and rebuilding. The auto
> setup downloads CUDA/Vulkan builds when a compatible GPU is detected, but still
> runs CPU inference by default.

Download the **CUDA 12.x** release of llama.cpp:
<https://github.com/ggerganov/llama.cpp/releases>
File: `llama-<version>-bin-win-cuda-cu12.x.x-x64.zip`

Files to place in **`ai\` next to `nameclean.exe`**:

| File | Source |
|------|--------|
| `llama-server.exe` | llama.cpp release zip |
| `ggml.dll` | llama.cpp release zip |
| `llama.dll` | llama.cpp release zip |
| `ggml-cuda.dll` (or `ggml_cuda.dll`) | llama.cpp release zip |
| `cudart64_12.dll` | CUDA runtime (see below) |
| `cublas64_12.dll` | CUDA runtime (see below) |
| `cublasLt64_12.dll` | CUDA runtime (see below) |
| `qwen2.5-3b-instruct-q4_k_m.gguf` | HuggingFace (see below) |

**CUDA runtime DLLs** can be obtained from either:
- The CUDA Toolkit 12.x installer (full install or runtime-only component), or
- The `cuda_runtime` package from the llama.cpp release zip (some releases bundle them).

**Requirement:** NVIDIA driver ≥ 525 with CUDA 12 support (GTX 900 series or newer).

> **Note:** The app launches with `--n-gpu-layers 0` (CPU-only) by default. To enable GPU
> offload, change this flag in `llm.c` and rebuild. The 3B Q4_K_M model (~2 GB) requires
> approximately 3 GB VRAM for full GPU offload.

---

## Model file

**Qwen2.5-3B-Instruct-Q4_K_M.gguf** (~2 GB)

Download from HuggingFace:
```
https://huggingface.co/Qwen/Qwen2.5-3B-Instruct-GGUF
```
File: `qwen2.5-3b-instruct-q4_k_m.gguf`

Place it in the **`ai\` folder** next to `nameclean.exe`. The app discovers any `*.gguf` file
in that directory automatically — filename does not matter.

Both `setup.bat` and the in-app setup wizard handle this download automatically. All downloads use a local `tmp\` directory (not the system temp folder), so no AppData access is required.

---

## Minimal distribution layout

```
nameclean\
  nameclean.exe                              ← main app (no extra DLLs needed)

  [optional — custom logo]
  config\
    logo.png                                 ← PNG logo, 621×100 px recommended

  [optional — AI features, all in ai\ subfolder]
  ai\
    llama-server.exe                         ← llama.cpp server
    ggml.dll                                 ← only if llama-server needs it
    llama.dll                                ← only if llama-server needs it
    ggml-cuda.dll                            ← CUDA build only
    cudart64_12.dll                          ← CUDA build only
    cublas64_12.dll                          ← CUDA build only
    cublasLt64_12.dll                        ← CUDA build only
    qwen2.5-3b-instruct-q4_k_m.gguf        ← AI model (~2 GB)

  [transient — created during setup, auto-cleaned]
  tmp\                                       ← local temp dir (not AppData)
```

---

## Verifying DLL dependencies after a rebuild

```bash
# In MSYS2 / MinGW shell, from the project root:
objdump -p nameclean.exe | grep "DLL Name"

# For llama-server.exe (after downloading):
objdump -p llama-server.exe | grep "DLL Name"
```

To statically link the MinGW C runtime (eliminates any future UCRT questions entirely):

```makefile
# In Makefile, append to LDFLAGS:
LDFLAGS += -static-libgcc
```

This produces a slightly larger exe but guarantees zero runtime DLL concerns on any
Windows version without needing the UCRT update.
