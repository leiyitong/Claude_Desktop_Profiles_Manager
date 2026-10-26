@echo off
rem Builds build\ClaudeDesktopProfilesManager.exe and runs the unit tests.
rem Needs Visual Studio 2022 or later (or its Build Tools) with the "Desktop development with C++" workload.
rem A test started beside the others (:Beside) runs in a cmd of its own, through this file.
if "%~1"=="--beside" goto :RunBeside
setlocal EnableExtensions

set "SELF=%~f0"

set "ROOT=%~dp0"
set "OUT=%ROOT%build"
set "EXE_NAME=ClaudeDesktopProfilesManager.exe"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

rem The exe is linked in obj\ and moved to build\ once every check and test passed: sign.cmd signs
rem build\ClaudeDesktopProfilesManager.exe, so a failed or older build must not stay there.
del /q "%OUT%\%EXE_NAME%" >nul 2>&1
if exist "%OUT%\%EXE_NAME%" (
    echo "%OUT%\%EXE_NAME%" cannot be replaced: close it first.
    exit /b 1
)

if not exist "%VSWHERE%" (
    echo vswhere.exe not found: install Visual Studio or its Build Tools with the C++ workload.
    exit /b 1
)
set "VSDIR="
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -version "[17.0,)" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo Visual Studio 2022 or later with its C++ build tools was not found.
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || (
    echo Loading the x64 compiler failed: run "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" to see why.
    exit /b 1
)
where cl.exe >nul 2>&1 && where rc.exe >nul 2>&1 || (
    echo The x64 compiler or the Windows SDK is not set up: run "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" to see why.
    exit /b 1
)

if not exist "%OUT%\obj" mkdir "%OUT%\obj"
if not exist "%OUT%\test" mkdir "%OUT%\test"
rem The tests link every object: one left from a renamed or deleted source must not be among them.
del /q "%OUT%\obj\*.obj" "%OUT%\obj\app.res" "%OUT%\obj\%EXE_NAME%" >nul 2>&1

set "CFLAGS=/nologo /W4 /WX /O2 /MT /GS /guard:cf /sdl /utf-8 /DUNICODE /D_UNICODE"

rem What this build is, for the update check through GitHub's CLI (ghupdate.c): GitHub Actions' run number (the
rem pre-release's build-<n>), commit and repository; a local build is number 0, of its origin's repository.
set "BUILD_NUMBER=%GITHUB_RUN_NUMBER%"
if not defined BUILD_NUMBER set "BUILD_NUMBER=0"
set "BUILD_COMMIT=%GITHUB_SHA%"
if not defined BUILD_COMMIT for /f "usebackq delims=" %%c in (`git -C "%ROOT%." rev-parse HEAD 2^>nul`) do set "BUILD_COMMIT=%%c"
if defined BUILD_COMMIT set "BUILD_COMMIT=%BUILD_COMMIT:~0,7%"
set "BUILD_REPO=%GITHUB_REPOSITORY%"
if not defined BUILD_REPO for /f "usebackq delims=" %%u in (`git -C "%ROOT%." remote get-url origin 2^>nul`) do set "BUILD_REPO=%%u"
if defined BUILD_REPO call :RepoOfUrl
> "%OUT%\obj\buildinfo.h" echo /* Written by build.cmd: what this build is (ghupdate.c). */
>> "%OUT%\obj\buildinfo.h" echo #define APP_BUILD_NUMBER %BUILD_NUMBER%
>> "%OUT%\obj\buildinfo.h" echo #define APP_BUILD_COMMIT L"%BUILD_COMMIT%"
>> "%OUT%\obj\buildinfo.h" echo #define APP_BUILD_REPO L"%BUILD_REPO%"
set "CFLAGS=%CFLAGS% /I"%OUT%\obj""
set "LIBS=user32.lib gdi32.lib shell32.lib ole32.lib comctl32.lib advapi32.lib uuid.lib uxtheme.lib dwmapi.lib winhttp.lib oleaut32.lib wintrust.lib crypt32.lib windowscodecs.lib rstrtmgr.lib bcrypt.lib comdlg32.lib ws2_32.lib iphlpapi.lib"

rem windows-mcp-delta's source, zipped into build\ for app.rc to embed (Windows MCP extracts and runs it).
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%tools\pack-delta.ps1" "%OUT%\windows-mcp-delta.zip" || exit /b 1

pushd "%ROOT%src" || exit /b 1
rc /nologo /fo "%OUT%\obj\app.res" app.rc
if errorlevel 1 (popd & exit /b 1)
popd

rem /MP: the sources compile in parallel, one per processor.
cl %CFLAGS% /MP /Fo"%OUT%\obj\\" /Fe"%OUT%\obj\%EXE_NAME%" "%ROOT%src\*.c" "%OUT%\obj\app.res" ^
   /link /SUBSYSTEM:WINDOWS /GUARD:CF /DYNAMICBASE /NXCOMPAT /DEPENDENTLOADFLAG:0x800 /OPT:REF /OPT:ICF %LIBS% || exit /b 1

rem Every interface text has its translations, and the two changelogs are in step. Needs Python 3.6 or later, which GitHub Actions has;
rem the python.org installer may put only "py" on PATH, and the Microsoft Store's "python" placeholder fails.
rem The checks outside parentheses: "a || exit /b 1" inside a block ends "cmd /c build.cmd" with code 0.
set "PYTHON="
python -c "import sys; sys.exit(sys.version_info < (3, 6))" >nul 2>&1 && set "PYTHON=python"
if not defined PYTHON (py -3 -c "import sys; sys.exit(sys.version_info < (3, 6))" >nul 2>&1 && set "PYTHON=py -3")
if not defined PYTHON goto :NoPython
%PYTHON% "%ROOT%tools\check-localization.py" || exit /b 1
%PYTHON% "%ROOT%tools\changelog.py" --check || exit /b 1
goto :Checked
:NoPython
echo Python 3.6 or later not found: tools\check-localization.py and tools\changelog.py skipped.
:Checked

rem The pure helpers alone: core.c needs nothing else.
cl %CFLAGS% /Fo"%OUT%\test\\" /Fe"%OUT%\test\test_core.exe" "%ROOT%tests\test_core.c" "%OUT%\obj\core.obj" ^
   /link /SUBSYSTEM:CONSOLE %LIBS% || exit /b 1
"%OUT%\test\test_core.exe" || exit /b 1

rem Complete interface catalogs, format arguments and language selection.
call :Test test_localize "" || exit /b 1
rem Pin entries, records and their commit, on private values and files.
call :Test test_pin "" || exit /b 1
rem Session storage and copies in private temporary fixtures, the longest to run: beside the tests
rem below, which share none of their files, and collected before test_claude.
call :Compile test_sessions "/MANIFEST:NO" "%OUT%\obj\app.res" || exit /b 1
call :Beside test_sessions
rem Reading copies, local hit context and metadata browsers.
call :Test test_sessiontools "" || exit /b 1
call :Test test_presets "" || exit /b 1
rem Sign-in cookies read from a WebView2 store and written into a profile's, with real winsqlite3, DPAPI and v10.
call :Test test_cookies "" || exit /b 1
rem Update, installation, watcher, routing and removal failures, with private files and controlled callbacks.
call :Test test_platform "" || exit /b 1
rem windows-mcp-delta written into a throwaway profile, extracted from the embedded ZIP (with the program's resources).
call :Test test_windowsmcp "/MANIFEST:NO" "%OUT%\obj\app.res" || exit /b 1
rem Shortcut COM failures use private files without launching their targets.
call :Test test_shortcuts "" || exit /b 1
rem What theme.c draws, against Windows' own drawing (with the program's manifest).
call :Test test_theme "/MANIFEST:NO" "%OUT%\obj\app.res" || exit /b 1
rem Native resource captions, wrapping and control bounds in every interface language (with the program's manifest).
call :Test test_layout "/MANIFEST:NO" "%OUT%\obj\app.res" || exit /b 1
call :Collect test_sessions || exit /b 1
rem What the program relies on in the installed Claude Desktop (skipped without it).
call :Test test_claude "" || exit /b 1

move /y "%OUT%\obj\%EXE_NAME%" "%OUT%\%EXE_NAME%" >nul || exit /b 1
echo.
echo Built "%OUT%\%EXE_NAME%"
exit /b 0

rem BUILD_REPO, a remote's URL, made the repository's "owner/name". Called only when it is set: a substring of an
rem unset variable would throw the line's percent signs out of their pairs.
:RepoOfUrl
set "BUILD_REPO=%BUILD_REPO:https://github.com/=%"
set "BUILD_REPO=%BUILD_REPO:git@github.com:=%"
if "%BUILD_REPO:~-1%"=="/" set "BUILD_REPO=%BUILD_REPO:~0,-1%"
if /i "%BUILD_REPO:~-4%"==".git" set "BUILD_REPO=%BUILD_REPO:~0,-4%"
exit /b 0

rem Builds tests\%1.c with the program's objects. %2: linker options; %3: one more input, quoted.
:Compile
cl %CFLAGS% /Fo"%OUT%\test\\" /Fe"%OUT%\test\%~1.exe" "%ROOT%tests\%~1.c" "%OUT%\obj\*.obj" %3 ^
   /link /SUBSYSTEM:CONSOLE %~2 %LIBS% || exit /b 1
exit /b 0

rem Builds tests\%1.c and runs it, as :Compile takes them.
:Test
call :Compile %1 %2 %3 || exit /b 1
"%OUT%\test\%~1.exe" || exit /b 1
exit /b 0

rem Starts the built test %1 beside the next ones: its output goes to %1.log, then its exit code to %1.exit.
:Beside
del /q "%OUT%\test\%~1.log" "%OUT%\test\%~1.exit" "%OUT%\test\%~1.code" >nul 2>&1
start "" /b cmd /d /c ""%SELF%" --beside %~1"
exit /b 0

rem In the cmd :Beside started, with the build's variables: test %2, its exit code written once it ended.
:RunBeside
"%OUT%\test\%~2.exe" > "%OUT%\test\%~2.log" 2>&1
> "%OUT%\test\%~2.code" echo %errorlevel%
move /y "%OUT%\test\%~2.code" "%OUT%\test\%~2.exit" >nul
exit /b 0

rem Waits for test %1 that :Beside started (ten minutes at most), shows what it wrote and fails with it.
:Collect
set "WAITED=0"
:CollectWait
if exist "%OUT%\test\%~1.exit" goto :Collected
if %WAITED% geq 600 (
    echo %~1 did not end within ten minutes.
    exit /b 1
)
set /a "WAITED+=1"
rem A second: ping waits between its two echoes.
ping -n 2 127.0.0.1 >nul
goto :CollectWait
:Collected
type "%OUT%\test\%~1.log"
set "CODE="
set /p "CODE=" < "%OUT%\test\%~1.exit"
if not "%CODE: =%"=="0" exit /b 1
exit /b 0
