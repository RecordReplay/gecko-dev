@ECHO OFF

REM This script is based on the mozilla-build start-shell.bat, but performs the
REM build and package steps instead of loading an interactive shell.

SETLOCAL ENABLEEXTENSIONS ENABLEDELAYEDEXPANSION

REM Reset some env vars.
SET CYGWIN=
SET INCLUDE=
SET LIB=
SET GITDIR=

SET MOZILLABUILD=C:\mozilla-build\

REM Find the Git bin directory so we can add it to the PATH.
IF NOT DEFINED MOZ_NO_GIT_DETECT (
  REM Try Windows PATH first
  FOR /F "tokens=*" %%A IN ('where git 2^>NUL') DO SET GITDIR=%%~dpA
  REM Current User 64-bit
  IF NOT DEFINED GITDIR (
    FOR /F "tokens=2*" %%A IN ('REG QUERY HKCU\Software\GitForWindows /v InstallPath 2^>NUL') DO SET "GITDIR=%%B\bin"
  )
  REM Current User 32-bit
  IF NOT DEFINED GITDIR (
    FOR /F "tokens=2*" %%A IN ('REG QUERY HKCU\Software\Wow6432Node\GitForWindows /v InstallPath 2^>NUL') DO SET "GITDIR=%%B\bin"
  )
  REM Local Machine 64-bit
  IF NOT DEFINED GITDIR (
    FOR /F "tokens=2*" %%A IN ('REG QUERY HKLM\Software\GitForWindows /v InstallPath 2^>NUL') DO SET "GITDIR=%%B\bin"
  )
  REM Local Machine User 32-bit
  IF NOT DEFINED GITDIR (
    FOR /F "tokens=2*" %%A IN ('REG QUERY HKLM\Software\Wow6432Node\GitForWindows /v InstallPath 2^>NUL') DO SET "GITDIR=%%B\bin"
  )
)

REM Reset to a known clean path, appending the path to Git if we found it.
IF NOT DEFINED MOZ_NO_RESET_PATH (
  SET PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem
)
IF DEFINED GITDIR (
  SET "PATH=%PATH%;!GITDIR!"
  SET GITDIR=
)

REM Start shell.
%MOZILLABUILD%msys\bin\bash --login recordreplay/%GECKODIR%/windows-build.sh

EXIT /B
