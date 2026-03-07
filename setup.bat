@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0"

echo ============================================================
echo  Address Cleaner - AI Setup
echo ============================================================
echo.

:: ---------------------------------------------------------------
:: 1. Skip if already set up
:: ---------------------------------------------------------------
if exist "ai\llama-server.exe" (
    for %%G in (ai\*.gguf) do (
        echo Already set up.  Delete the ai\ folder to re-run.
        echo.
        pause
        exit /b 0
    )
)

:: ---------------------------------------------------------------
:: 2. Detect GPU using PowerShell (avoids deprecated wmic)
::    Writes detected mode to a temp file: cuda / vulkan / cpu
:: ---------------------------------------------------------------
set PS_TMP=%TEMP%\llama_ps.ps1
set MODE_TMP=%TEMP%\llama_mode.txt
set API_JSON=%TEMP%\llama_api.json
set URL_TMP=%TEMP%\llama_url.txt

del "%MODE_TMP%" >nul 2>&1

(
    echo $gpus = ^(Get-CimInstance Win32_VideoController^).Name -join ' '
    echo if ^($gpus -match 'NVIDIA'^) { Set-Content -Path '%MODE_TMP%' -Value 'cuda' }
    echo elseif ^($gpus -match 'Radeon'^) { Set-Content -Path '%MODE_TMP%' -Value 'vulkan' }
    echo elseif ^($gpus -match 'AMD'^) { Set-Content -Path '%MODE_TMP%' -Value 'vulkan' }
    echo else { Set-Content -Path '%MODE_TMP%' -Value 'cpu' }
) > "%PS_TMP%"

powershell -NoProfile -ExecutionPolicy Bypass -File "%PS_TMP%" >nul 2>&1
del "%PS_TMP%" >nul 2>&1

set MODE=cpu
if exist "%MODE_TMP%" (
    set /p MODE=<"%MODE_TMP%"
    del "%MODE_TMP%" >nul 2>&1
)

if "!MODE!"=="cuda"   echo NVIDIA GPU detected - downloading CUDA build.
if "!MODE!"=="vulkan" echo AMD GPU detected - downloading Vulkan build.
if "!MODE!"=="cpu"    echo No dedicated GPU - downloading CPU build.
echo.

:: ---------------------------------------------------------------
:: 3. Fetch GitHub API JSON with curl (handles TLS correctly)
:: ---------------------------------------------------------------
echo Fetching latest llama.cpp release info...

curl.exe -s -L -H "User-Agent: setup.bat" -o "%API_JSON%" ^
    "https://api.github.com/repos/ggml-org/llama.cpp/releases/latest"

if %errorlevel% neq 0 (
    echo ERROR: Could not reach GitHub API. Check your internet connection.
    pause & exit /b 1
)

findstr /C:"browser_download_url" "%API_JSON%" >nul 2>&1
if %errorlevel% neq 0 (
    echo ERROR: GitHub API returned unexpected response.
    echo You may have hit the rate limit ^(60 requests/hour^). Try again later.
    del "%API_JSON%" >nul 2>&1
    pause & exit /b 1
)

:: ---------------------------------------------------------------
:: 4. Parse JSON with a temp PowerShell script (no inline escaping)
::    Current naming: llama-bNNNN-bin-win-{cpu|vulkan|cuda-12.x}-x64.zip
:: ---------------------------------------------------------------
if "!MODE!"=="cuda"   set PATTERN=bin-win-cuda-12
if "!MODE!"=="vulkan" set PATTERN=bin-win-vulkan-x64
if "!MODE!"=="cpu"    set PATTERN=bin-win-cpu-x64

del "%URL_TMP%" >nul 2>&1

(
    echo $j = ConvertFrom-Json ^(Get-Content -Raw '%API_JSON%'^)
    echo $urls = $j.assets.browser_download_url
    echo $u = $urls.Where^({$_ -match '!PATTERN!'}^)[0]
    echo if ^($u^) { Set-Content -Path '%URL_TMP%' -Value $u -NoNewline }
) > "%PS_TMP%"

powershell -NoProfile -ExecutionPolicy Bypass -File "%PS_TMP%" >nul 2>&1
del "%PS_TMP%" >nul 2>&1
del "%API_JSON%" >nul 2>&1

set LLAMA_URL=
if exist "%URL_TMP%" (
    set /p LLAMA_URL=<"%URL_TMP%"
    del "%URL_TMP%" >nul 2>&1
)

if "!LLAMA_URL!"=="" (
    echo.
    echo ERROR: Could not find a matching download URL.
    echo   Pattern searched: !PATTERN!
    echo.
    echo Visit https://github.com/ggml-org/llama.cpp/releases
    echo Download the Windows zip manually and extract into the ai\ folder.
    echo.
    pause & exit /b 1
)

echo Found: !LLAMA_URL!
echo.

:: ---------------------------------------------------------------
:: 5. Download llama.cpp zip
:: ---------------------------------------------------------------
set LLAMA_ZIP=%TEMP%\llama_setup.zip
set LLAMA_TMP=%TEMP%\llama_setup

echo Downloading llama.cpp [!MODE! build]...
curl.exe -L --progress-bar -o "%LLAMA_ZIP%" "!LLAMA_URL!"
if %errorlevel% neq 0 (
    echo ERROR: Download failed.
    pause & exit /b 1
)
echo.

:: ---------------------------------------------------------------
:: 6. Extract into ai\
:: ---------------------------------------------------------------
echo Extracting...
if exist "%LLAMA_TMP%" rd /s /q "%LLAMA_TMP%"
powershell -NoProfile -Command "Expand-Archive -Path '%LLAMA_ZIP%' -DestinationPath '%LLAMA_TMP%' -Force"
if %errorlevel% neq 0 (
    echo ERROR: Extraction failed.
    pause & exit /b 1
)

if not exist "ai" mkdir ai

:: Files may be in a subfolder inside the zip — find the right source dir
set LLAMA_SRC=%LLAMA_TMP%
for /d %%D in ("%LLAMA_TMP%\*") do (
    if exist "%%D\llama-server.exe" set LLAMA_SRC=%%D
)

:: Copy everything — server exe, all DLLs
copy /Y "%LLAMA_SRC%\llama-server.exe" "ai\" >nul 2>&1 && echo   Copied llama-server.exe
for %%F in ("%LLAMA_SRC%\*.dll") do (
    copy /Y "%%F" "ai\" >nul 2>&1
    echo   Copied %%~nxF
)

rd /s /q "%LLAMA_TMP%" >nul 2>&1
del "%LLAMA_ZIP%" >nul 2>&1

if not exist "ai\llama-server.exe" (
    echo.
    echo WARNING: llama-server.exe was not found in the extracted files.
    echo Check the releases page and extract manually into ai\
    echo.
)

:: ---------------------------------------------------------------
:: 7. Download model
:: ---------------------------------------------------------------
set HAS_MODEL=0
for %%G in (ai\*.gguf) do set HAS_MODEL=1

if "!HAS_MODEL!"=="0" (
    echo.
    echo Downloading Qwen2.5-3B model  ~2 GB, please wait...
    curl.exe -L --progress-bar ^
        -o "ai\qwen2.5-3b-instruct-q4_k_m.gguf" ^
        "https://huggingface.co/Qwen/Qwen2.5-3B-Instruct-GGUF/resolve/main/qwen2.5-3b-instruct-q4_k_m.gguf"
    if %errorlevel% neq 0 (
        echo ERROR: Model download failed.
        echo Download manually: https://huggingface.co/Qwen/Qwen2.5-3B-Instruct-GGUF
        pause & exit /b 1
    )
) else (
    echo Model already present - skipping download.
)

:: ---------------------------------------------------------------
:: 8. Summary
:: ---------------------------------------------------------------
echo.
echo ============================================================
echo  Setup complete!  [!MODE! build]
echo ============================================================
if exist "ai\llama-server.exe" echo   ai\llama-server.exe .... OK
for %%G in (ai\*.gguf) do echo   ai\%%G .... OK
echo.
echo Launch nameclean.exe - AI will start automatically.
echo Status bar will show "AI: Loading..." then "AI: Ready".
echo.
pause
