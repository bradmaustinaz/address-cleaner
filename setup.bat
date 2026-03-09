@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0"

:: ---------------------------------------------------------------
:: Diagnostic log setup
:: ---------------------------------------------------------------
set LOGFILE=%~dp0setup_log.txt
echo [%date% %time%] Setup started > "%LOGFILE%"
echo [%date% %time%] Arguments: %* >> "%LOGFILE%"

:: ---------------------------------------------------------------
:: Parse command-line flags
:: ---------------------------------------------------------------
set OFFLINE=0
for %%A in (%*) do (
    if /I "%%A"=="--offline" set OFFLINE=1
    if /I "%%A"=="-offline"  set OFFLINE=1
)

echo ============================================================
echo  Address Cleaner - AI Setup
echo ============================================================
echo.

if "!OFFLINE!"=="1" (
    echo Mode: OFFLINE ^(skipping all downloads^)
    echo [%date% %time%] Offline mode selected >> "%LOGFILE%"
    echo.
    goto :offline_instructions
)

:: ---------------------------------------------------------------
:: 1. Skip if already set up
:: ---------------------------------------------------------------
if exist "ai\llama-server.exe" (
    for %%G in (ai\*.gguf) do (
        echo Already set up.  Delete the ai\ folder to re-run.
        echo [%date% %time%] Already set up - exiting >> "%LOGFILE%"
        echo.
        pause
        exit /b 0
    )
)

:: ---------------------------------------------------------------
:: 2. Network connectivity check (auto-detect restricted networks)
:: ---------------------------------------------------------------
echo [Step 1/6] Checking network connectivity...
echo [%date% %time%] Checking network connectivity >> "%LOGFILE%"

curl.exe -s -o nul -w "%%{http_code}" --connect-timeout 5 --max-time 10 "https://api.github.com/rate_limit" > "%TEMP%\llama_netcheck.txt" 2>"%TEMP%\llama_neterr.txt"
set CURL_EXIT=%errorlevel%
set HTTP_CODE=000
if exist "%TEMP%\llama_netcheck.txt" set /p HTTP_CODE=<"%TEMP%\llama_netcheck.txt"

echo [%date% %time%] Network check: curl exit=%CURL_EXIT%, HTTP=%HTTP_CODE% >> "%LOGFILE%"

if %CURL_EXIT% neq 0 (
    echo.
    echo WARNING: Cannot reach GitHub API ^(curl exit code: %CURL_EXIT%^).
    echo [%date% %time%] Network check failed: curl exit %CURL_EXIT% >> "%LOGFILE%"

    :: Diagnose specific failure modes
    if %CURL_EXIT%==6 (
        echo   Cause: DNS resolution failed.
        echo   Fix:   Check DNS settings or try a public DNS ^(8.8.8.8^).
        echo [%date% %time%] Diagnosis: DNS resolution failure >> "%LOGFILE%"
    )
    if %CURL_EXIT%==7 (
        echo   Cause: Connection refused or blocked by firewall/proxy.
        echo   Fix:   Check firewall rules or configure proxy with:
        echo          set HTTPS_PROXY=http://your-proxy:port
        echo [%date% %time%] Diagnosis: Connection refused/firewall >> "%LOGFILE%"
    )
    if %CURL_EXIT%==28 (
        echo   Cause: Connection timed out.
        echo   Fix:   Check internet connection or proxy settings.
        echo [%date% %time%] Diagnosis: Connection timeout >> "%LOGFILE%"
    )
    if %CURL_EXIT%==35 (
        echo   Cause: TLS/SSL handshake failed ^(possible TLS inspection^).
        echo   Fix:   Contact IT to whitelist github.com or provide CA cert.
        echo [%date% %time%] Diagnosis: TLS handshake failure >> "%LOGFILE%"
    )
    if %CURL_EXIT%==60 (
        echo   Cause: SSL certificate problem ^(corporate TLS inspection^).
        echo   Fix:   Contact IT for the corporate CA certificate.
        echo [%date% %time%] Diagnosis: SSL certificate issue >> "%LOGFILE%"
    )

    echo.
    echo Switching to offline mode automatically.
    echo [%date% %time%] Auto-switching to offline mode >> "%LOGFILE%"
    echo.
    goto :offline_instructions
)

if "!HTTP_CODE!"=="403" (
    echo   GitHub API rate limit reached. Retrying may work later.
    echo [%date% %time%] GitHub rate-limited ^(403^) >> "%LOGFILE%"
)

if "!HTTP_CODE:~0,1!"=="2" (
    echo   Network OK.
    echo [%date% %time%] Network check passed >> "%LOGFILE%"
) else (
    echo   Unexpected HTTP response: !HTTP_CODE!
    echo   The network may be restricted ^(proxy auth, captive portal^).
    echo [%date% %time%] Unexpected HTTP %HTTP_CODE% >> "%LOGFILE%"
    echo.
    echo Switching to offline mode automatically.
    echo [%date% %time%] Auto-switching to offline mode >> "%LOGFILE%"
    echo.
    goto :offline_instructions
)

del "%TEMP%\llama_netcheck.txt" >nul 2>&1
del "%TEMP%\llama_neterr.txt" >nul 2>&1
echo.

:: ---------------------------------------------------------------
:: 3. Detect GPU using PowerShell (avoids deprecated wmic)
:: ---------------------------------------------------------------
echo [Step 2/6] Detecting GPU...
echo [%date% %time%] Detecting GPU >> "%LOGFILE%"

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
set PS_EXIT=%errorlevel%
del "%PS_TMP%" >nul 2>&1

set MODE=cpu
if exist "%MODE_TMP%" (
    set /p MODE=<"%MODE_TMP%"
    del "%MODE_TMP%" >nul 2>&1
)

if %PS_EXIT% neq 0 (
    echo   PowerShell detection failed ^(exit %PS_EXIT%^) - defaulting to CPU build.
    echo [%date% %time%] PowerShell GPU detection failed, defaulting to cpu >> "%LOGFILE%"
    set MODE=cpu
)

echo [%date% %time%] GPU mode: !MODE! >> "%LOGFILE%"

if "!MODE!"=="cuda"   echo   NVIDIA GPU detected - will download CUDA build.
if "!MODE!"=="vulkan" echo   AMD GPU detected - will download Vulkan build.
if "!MODE!"=="cpu"    echo   No dedicated GPU detected - will download CPU build.
echo.

:: ---------------------------------------------------------------
:: 4. Fetch GitHub API JSON with curl (timeout-wrapped)
:: ---------------------------------------------------------------
echo [Step 3/6] Fetching latest llama.cpp release info...
echo [%date% %time%] Fetching GitHub release info >> "%LOGFILE%"

curl.exe -s -L --connect-timeout 5 --max-time 15 -H "User-Agent: address-cleaner-setup" -o "%API_JSON%" ^
    "https://api.github.com/repos/ggml-org/llama.cpp/releases/latest"

if %errorlevel% neq 0 (
    echo.
    echo ERROR: Could not fetch release info from GitHub ^(exit code: %errorlevel%^).
    echo [%date% %time%] GitHub API fetch failed: exit %errorlevel% >> "%LOGFILE%"
    echo.
    echo Switching to offline mode.
    goto :offline_instructions
)

findstr /C:"browser_download_url" "%API_JSON%" >nul 2>&1
if %errorlevel% neq 0 (
    echo ERROR: GitHub API returned unexpected response.
    echo   You may have hit the rate limit ^(60 requests/hour^).
    echo [%date% %time%] GitHub API: no download URLs in response >> "%LOGFILE%"
    echo.
    echo Switching to offline mode.
    goto :offline_instructions
)

echo   Release info retrieved.
echo.

:: ---------------------------------------------------------------
:: 5. Parse JSON for download URL
:: ---------------------------------------------------------------
echo [Step 4/6] Finding download URL for !MODE! build...

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
    echo [%date% %time%] No matching URL for pattern: !PATTERN! >> "%LOGFILE%"
    echo.
    echo Switching to offline mode.
    goto :offline_instructions
)

echo   Found: !LLAMA_URL!
echo [%date% %time%] Download URL: !LLAMA_URL! >> "%LOGFILE%"
echo.

:: ---------------------------------------------------------------
:: 6. Download llama.cpp zip (with timeout)
:: ---------------------------------------------------------------
set LLAMA_ZIP=%TEMP%\llama_setup.zip
set LLAMA_TMP=%TEMP%\llama_setup

echo [Step 5/6] Downloading llama.cpp [!MODE! build]...
echo   This may take a few minutes depending on your connection.
echo [%date% %time%] Downloading llama.cpp zip >> "%LOGFILE%"

curl.exe -L --connect-timeout 10 --max-time 300 --progress-bar -o "%LLAMA_ZIP%" "!LLAMA_URL!"
if %errorlevel% neq 0 (
    echo.
    echo ERROR: Download failed ^(exit code: %errorlevel%^).
    echo [%date% %time%] llama.cpp download failed: exit %errorlevel% >> "%LOGFILE%"
    echo.
    echo Switching to offline mode.
    goto :offline_instructions
)
echo.

:: ---------------------------------------------------------------
:: 7. Extract into ai\
:: ---------------------------------------------------------------
echo [Step 6/6] Extracting files...
echo [%date% %time%] Extracting llama.cpp >> "%LOGFILE%"

if exist "%LLAMA_TMP%" rd /s /q "%LLAMA_TMP%"
powershell -NoProfile -Command "Expand-Archive -Path '%LLAMA_ZIP%' -DestinationPath '%LLAMA_TMP%' -Force"
if %errorlevel% neq 0 (
    echo ERROR: Extraction failed.
    echo [%date% %time%] Extraction failed >> "%LOGFILE%"
    pause & exit /b 1
)

if not exist "ai" mkdir ai

:: Files may be in a subfolder inside the zip
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
    echo [%date% %time%] WARNING: llama-server.exe not found after extraction >> "%LOGFILE%"
    echo Check the releases page and extract manually into the ai\ folder.
    echo.
)

:: ---------------------------------------------------------------
:: 8. Download model
:: ---------------------------------------------------------------
set HAS_MODEL=0
for %%G in (ai\*.gguf) do set HAS_MODEL=1

if "!HAS_MODEL!"=="0" (
    echo.
    echo Downloading Qwen2.5-3B model  ~2 GB, please wait...
    echo [%date% %time%] Downloading model >> "%LOGFILE%"
    curl.exe -L --connect-timeout 10 --max-time 600 --progress-bar ^
        -o "ai\qwen2.5-3b-instruct-q4_k_m.gguf" ^
        "https://huggingface.co/Qwen/Qwen2.5-3B-Instruct-GGUF/resolve/main/qwen2.5-3b-instruct-q4_k_m.gguf"
    if %errorlevel% neq 0 (
        echo.
        echo ERROR: Model download failed.
        echo [%date% %time%] Model download failed >> "%LOGFILE%"
        echo.
        echo You can download the model manually:
        echo   URL:  https://huggingface.co/Qwen/Qwen2.5-3B-Instruct-GGUF/resolve/main/qwen2.5-3b-instruct-q4_k_m.gguf
        echo   Save: ai\qwen2.5-3b-instruct-q4_k_m.gguf
        echo.
        pause & exit /b 1
    )
) else (
    echo Model already present - skipping download.
)

echo [%date% %time%] Setup completed successfully >> "%LOGFILE%"
goto :summary

:: ===============================================================
:: OFFLINE MODE: Manual installation instructions
:: ===============================================================
:offline_instructions
echo.
echo ============================================================
echo  OFFLINE / RESTRICTED NETWORK SETUP
echo ============================================================
echo.
echo Your network cannot reach the required download servers.
echo Please follow these steps on a machine with internet access:
echo.
echo 1. Download llama.cpp for Windows:
echo    https://github.com/ggml-org/llama.cpp/releases/latest
echo    Choose the zip matching your GPU:
echo      - NVIDIA:  llama-*-bin-win-cuda-12*-x64.zip
echo      - AMD:     llama-*-bin-win-vulkan-x64.zip
echo      - CPU:     llama-*-bin-win-cpu-x64.zip
echo.
echo 2. Download the AI model (~2 GB):
echo    https://huggingface.co/Qwen/Qwen2.5-3B-Instruct-GGUF/resolve/main/qwen2.5-3b-instruct-q4_k_m.gguf
echo.
echo 3. Create an "ai" folder next to this script:
echo    %~dp0ai\
echo.
echo 4. Place these files in the ai\ folder:
echo      ai\llama-server.exe   ^(from the zip^)
echo      ai\*.dll              ^(all DLLs from the zip^)
echo      ai\qwen2.5-3b-instruct-q4_k_m.gguf
echo.
echo 5. Run setup.bat again to verify, or launch nameclean.exe directly.
echo.
echo Diagnostic log saved to: %LOGFILE%
echo [%date% %time%] Offline instructions displayed >> "%LOGFILE%"
echo.
pause
exit /b 1

:: ---------------------------------------------------------------
:: Summary
:: ---------------------------------------------------------------
:summary
echo.
echo ============================================================
echo  Setup complete!  [!MODE! build]
echo ============================================================
if exist "ai\llama-server.exe" echo   ai\llama-server.exe .... OK
for %%G in (ai\*.gguf) do echo   ai\%%~nxG .... OK
echo.
echo Diagnostic log: %LOGFILE%
echo Launch nameclean.exe - AI will start automatically.
echo Status bar will show "AI: Loading..." then "AI: Ready".
echo.
pause
