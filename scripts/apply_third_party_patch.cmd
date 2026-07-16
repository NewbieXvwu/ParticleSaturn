@echo off
setlocal

cmake -DPARTICLESATURN_PATCH=%~1 -P "%~dp0apply_third_party_patch.cmake"
exit /b %errorlevel%
