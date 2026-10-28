@echo off
rem Remove build output and the collected bundle. Sources are not touched.
setlocal
set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"
echo Removing "%ROOT%\build-vs2022" ...
if exist "%ROOT%\build-vs2022" rmdir /s /q "%ROOT%\build-vs2022"
echo Removing "%ROOT%\dist" ...
if exist "%ROOT%\dist" rmdir /s /q "%ROOT%\dist"
echo Done.
endlocal
