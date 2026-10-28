@echo off
rem ===========================================================================
rem  OROGEN -- build VST3 with Visual Studio 2022 (x64)
rem
rem  Usage:   build_vst3.bat [Release|Debug]
rem
rem  What it does:
rem    1. finds CMake (own PATH first, then the one shipped with VS 2022)
rem    2. configures the project with the "Visual Studio 17 2022" generator
rem    3. builds the OROGEN_VST3 target
rem    4. copies the bundle to dist\OROGEN.vst3
rem
rem  JUCE 8.0.15 is downloaded by CMake (FetchContent) on the first run.
rem  If you already have JUCE, set JUCE_PATH before running:
rem      set JUCE_PATH=C:\JUCE
rem  To also install into the system VST3 folder:
rem      set INSTALL_VST3=1
rem
rem  On any error the window STAYS OPEN so the log can be read and copied.
rem  Set OROGEN_NO_PAUSE=1 to disable all pauses (for CI / scheduled runs).
rem ===========================================================================
setlocal EnableDelayedExpansion
set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"
if /i not "%CONFIG%"=="Release" if /i not "%CONFIG%"=="Debug" (
    echo [ERROR] Unknown configuration "%CONFIG%" -- use Release or Debug.
    set "ERRMSG=Unknown configuration %CONFIG%"
    goto :fail
)
set "BUILD_DIR=%ROOT%\build-vs2022"
set "DIST=%ROOT%\dist"
set "LOG=%BUILD_DIR%\last-build.log"

rem Was the script launched by a double click in Explorer (cmd /c ...)? Then keep
rem the window open after a successful build too, so the bundle path is visible.
set "DOUBLECLICK=0"
rem cmd /c ... == launched from Explorer. If find.exe is missing we assume
rem a double click: an extra pause is harmless, a lost log is not.
echo !cmdcmdline! | find /i "/c" >nul 2>nul
if errorlevel 2 set "DOUBLECLICK=1"
if not errorlevel 1 set "DOUBLECLICK=1"

echo.
echo ============================================================
echo  OROGEN -- VST3 build, Visual Studio 2022, x64, %CONFIG%
echo ============================================================

rem ---------- 1. CMake ------------------------------------------------------
set "CMAKE_EXE="
where cmake >nul 2>nul && for /f "delims=" %%i in ('where cmake') do if not defined CMAKE_EXE set "CMAKE_EXE=%%i"

if not defined CMAKE_EXE (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if exist "!VSWHERE!" (
        for /f "usebackq delims=" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.Component.MSBuild -property installationPath`) do (
            if exist "%%i\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
                set "CMAKE_EXE=%%i\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
            )
        )
    )
)

if not defined CMAKE_EXE (
    echo [ERROR] CMake not found.
    echo         Install "Desktop development with C++" in Visual Studio 2022
    echo         ^(it includes CMake^), or install CMake 3.22+ from cmake.org
    echo         and add it to PATH.
    set "ERRMSG=CMake not found"
    goto :fail
)
echo [1/4] CMake: "!CMAKE_EXE!"

rem ---------- 2. JUCE source ------------------------------------------------
set "JUCE_ARG="
if defined JUCE_PATH (
    if exist "%JUCE_PATH%\modules" (
        set "JUCE_ARG=-DOROGEN_JUCE_PATH=%JUCE_PATH%"
        echo [2/4] JUCE: local copy "%JUCE_PATH%"
    ) else (
        echo [WARN] JUCE_PATH is set but "%JUCE_PATH%\modules" does not exist -- falling back to download
    )
)
if not defined JUCE_ARG (
    where git >nul 2>nul
    if errorlevel 1 (
        echo [ERROR] git not found in PATH. CMake needs it to download JUCE 8.0.15.
        echo         Either install Git for Windows, or clone JUCE manually and set
        echo         JUCE_PATH, or unpack JUCE into a folder and set JUCE_PATH to it.
        set "ERRMSG=git not found in PATH"
        goto :fail
    )
    echo [2/4] JUCE: will be downloaded by CMake ^(first run only^)
)

rem ---------- 3. Configure --------------------------------------------------
echo [3/4] Configuring...
"!CMAKE_EXE!" -S "%ROOT%\orogen-plugin" -B "%BUILD_DIR%" ^
    -G "Visual Studio 17 2022" -A x64 ^
    -DOROGEN_BUILD_STANDS=OFF ^
    !JUCE_ARG!
if errorlevel 1 (
    echo.
    echo [ERROR] Configure failed. See messages above.
    set "ERRMSG=Configure failed (cmake -S ... -B build-vs2022)"
    goto :fail
)

rem ---------- 4. Build ------------------------------------------------------
echo [4/4] Building OROGEN_VST3 ^(%CONFIG%^)...
rem Build output also goes to a log file: on failure it can be attached as is.
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
"!CMAKE_EXE!" --build "%BUILD_DIR%" --config %CONFIG% --target OROGEN_VST3 --parallel > "%LOG%" 2>&1
set "BUILD_RC=!ERRORLEVEL!"
type "%LOG%"
if not "!BUILD_RC!"=="0" (
    echo.
    echo [ERROR] Build failed ^(exit code !BUILD_RC!^). See messages above.
    set "ERRMSG=Build failed, exit code !BUILD_RC!"
    goto :fail
)

rem ---------- copy result --------------------------------------------------
set "ART=%BUILD_DIR%\OROGEN_artefacts\%CONFIG%\VST3\OROGEN.vst3"
if not exist "%ART%" (
    echo [ERROR] Build reported success but "%ART%" is missing.
    set "ERRMSG=Artefact missing: %ART%"
    goto :fail
)

if not exist "%DIST%" mkdir "%DIST%"
if exist "%DIST%\OROGEN.vst3" rmdir /s /q "%DIST%\OROGEN.vst3"
xcopy "%ART%" "%DIST%\OROGEN.vst3" /E /I /Y >nul
if errorlevel 1 (
    echo [ERROR] Could not copy the bundle to "%DIST%".
    set "ERRMSG=Could not copy bundle to %DIST%"
    goto :fail
)

echo.
echo ------------------------------------------------------------
echo  OK. Bundle:
echo    %DIST%\OROGEN.vst3
echo  Build log:
echo    %LOG%
echo ------------------------------------------------------------

if defined INSTALL_VST3 (
    set "SYS_VST3=%COMMONPROGRAMFILES%\VST3"
    if not exist "!SYS_VST3!" mkdir "!SYS_VST3!"
    echo Installing to "!SYS_VST3!\OROGEN.vst3" ...
    if exist "!SYS_VST3!\OROGEN.vst3" rmdir /s /q "!SYS_VST3!\OROGEN.vst3"
    xcopy "%DIST%\OROGEN.vst3" "!SYS_VST3!\OROGEN.vst3" /E /I /Y >nul
    if errorlevel 1 (
        echo [WARN] Copy to Program Files failed -- run this script as Administrator,
        echo        or just drag dist\OROGEN.vst3 into your VST3 folder manually.
    ) else (
        echo Installed: "!SYS_VST3!\OROGEN.vst3"
    )
)

echo.
echo Next steps:
echo   * copy dist\OROGEN.vst3 to  C:\Program Files\Common Files\VST3\
echo     ^(or set INSTALL_VST3=1 and run as Administrator^)
echo   * rescan plugins in your DAW; OROGEN shows up as an instrument
echo.

rem The window closes immediately only when started from an already open console.
rem On a double click we wait for Enter, so the bundle path stays readable.
if "%DOUBLECLICK%"=="1" if not defined OROGEN_NO_PAUSE (
    echo Press any key to close...
    pause >nul
)
endlocal & exit /b 0

rem ===========================================================================
rem  Error path: print the reason and the log path, and KEEP THE WINDOW OPEN.
rem ===========================================================================
:fail
echo.
echo ============================================================
echo  BUILD FAILED: %ERRMSG%
echo ============================================================
if exist "%LOG%" (
    echo  Full build log:
    echo    %LOG%
    echo  ^(open it in Notepad, or attach the whole file when reporting^)
) else (
    echo  Log file was not created yet - the failure happened before the build step.
    echo  Copy the text above from the console window.
)
echo.
echo  How to retry:
echo    build_vst3.bat %CONFIG%
echo.
if not defined OROGEN_NO_PAUSE (
    echo  The window stays open. Press any key to close ^(or set OROGEN_NO_PAUSE=1^)...
    pause >nul
)
endlocal & exit /b 1
