@echo off
set "STATE=%~dp0.claude\hooks\state"
del "%STATE%\codex.disabled" 2>nul
echo Codex 协作已开启
