@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0"

echo ============================================================
echo  Address Cleaner — AI Setup
echo ============================================================
echo.

:: ---------------------------------------------------------------
:: 1. Skip if already set up
:: ---------------------------------------------------------------
if exist "llama-server.exe" (
    for %%G in (*.gguf) do (
        echo Already set up.  Delete llama-server.exe or the .gguf to re-run.
        echo.
        pause
        exit /b 0
    )
)

:: ---------------------------------------------------------------
:: 2. Detect CUDA
:: ---------------------------------------------------------------
set MODE=cpu
nvidia-smi >nul 2>&1
if %errorlevel%==0 (
    set MODE=cuda
    echo NVIDIA GPU detected — downloading CUDA build.
) else (
    echo No NVIDIA GPU found — downloading CPU build.
)
echo.

:: ---------------------------------------------------------------
:: 3. Fetch latest llama.cpp release URL from GitHub
:: ---------------------------------------------------------------
echo Fetching latest llama.cpp release info...

if "%MODE%"=="cuda" (
    set PS_FILTER=win.*cuda-cu12.*x64\.zip
) else (
    set PS_FILTER=win.*avx2.*x64\.zip
)

for /f "delims=" %%U in ('powershell -NoProfile -Command ^
    "try { $r=(Invoke-RestMethod 'https://api.github.com/repos/ggml-org/llama.cpp/releases/latest').assets.browser_download_url; $r ^| Where-Object {$_ -match '!PS_FILTER!'} ^| Select-Object -First 1 } catch { '' }"') do set LLAMA_URL=%%U

if "!LLAMA_URL!"=="" (
    echo ERROR: Could not fetch release URL from GitHub.
    echo Check your internet connection or visit:
    echo   https://github.com/ggml-org/llama.cpp/releases
    echo.
    pause
    exit /b 1
)

echo Found: !LLAMA_URL!
echo.

:: ---------------------------------------------------------------
:: 4. Download llama.cpp zip
:: ---------------------------------------------------------------
set LLAMA_ZIP=%TEMP%\llama-setup.zip
set LLAMA_TMP=%TEMP%\llama-setup

echo Downloading llama.cpp...
curl.exe -L --progress-bar -o "!LLAMA_ZIP!" "!LLAMA_URL!"
if %errorlevel% neq 0 (
    echo ERROR: Download failed.
    pause
    exit /b 1
)
echo.

:: ---------------------------------------------------------------
:: 5. Extract needed files
:: ---------------------------------------------------------------
echo Extracting files...
if exist "!LLAMA_TMP!" rd /s /q "!LLAMA_TMP!"

powershell -NoProfile -Command ^
    "Expand-Archive -Path '!LLAMA_ZIP!' -DestinationPath '!LLAMA_TMP!' -Force"

if %errorlevel% neq 0 (
    echo ERROR: Extraction failed.
    pause
    exit /b 1
)

:: Copy core files
for %%F in (llama-server.exe ggml.dll llama.dll) do (
    if exist "!LLAMA_TMP!\%%F" (
        copy /Y "!LLAMA_TMP!\%%F" "%%F" >nul
        echo   Copied %%F
    )
)

:: Copy CUDA DLLs if applicable
if "%MODE%"=="cuda" (
    for %%F in (cudart64_12.dll cublas64_12.dll cublasLt64_12.dll) do (
        if exist "!LLAMA_TMP!\%%F" (
            copy /Y "!LLAMA_TMP!\%%F" "%%F" >nul
            echo   Copied %%F
        )
    )
    for %%F in ("!LLAMA_TMP!\*cuda*.dll") do (
        copy /Y "%%F" . >nul 2>&1
        echo   Copied %%~nxF
    )
)

:: ---------------------------------------------------------------
:: 6. Cleanup zip / temp
:: ---------------------------------------------------------------
rd /s /q "!LLAMA_TMP!" >nul 2>&1
del "!LLAMA_ZIP!" >nul 2>&1

:: ---------------------------------------------------------------
:: 7. Download model (skip if any .gguf already present)
:: ---------------------------------------------------------------
set HAS_MODEL=0
for %%G in (*.gguf) do set HAS_MODEL=1

if "!HAS_MODEL!"=="0" (
    echo.
    echo Downloading Qwen2.5-1.5B model  ~940 MB, please wait...
    curl.exe -L --progress-bar ^
        -o "qwen2.5-1.5b-instruct-q4_k_m.gguf" ^
        "https://huggingface.co/Qwen/Qwen2.5-1.5B-Instruct-GGUF/resolve/main/qwen2.5-1.5b-instruct-q4_k_m.gguf"
    if %errorlevel% neq 0 (
        echo ERROR: Model download failed.
        echo Download manually from:
        echo   https://huggingface.co/Qwen/Qwen2.5-1.5B-Instruct-GGUF
        pause
        exit /b 1
    )
) else (
    echo Model already present — skipping download.
)

:: ---------------------------------------------------------------
:: 8. Summary
:: ---------------------------------------------------------------
echo.
echo ============================================================
echo  Setup complete!
echo ============================================================
if exist "llama-server.exe" echo   llama-server.exe .... OK
for %%G in (*.gguf) do       echo   %%G .... OK
echo.
echo Launch nameclean.exe — AI will start automatically.
echo Status bar will show "AI: Loading..." then "AI: Ready".
echo.
pause
