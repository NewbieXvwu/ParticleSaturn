@echo off
set "STATE=%~dp0.claude\hooks\state"
if not exist "%STATE%" mkdir "%STATE%"
type nul > "%STATE%\codex.disabled"
echo Codex 协作已关闭
