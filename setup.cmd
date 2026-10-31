@echo off
rem Agent Buddy setup for Windows: double-click this file, or run it in a terminal.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup.ps1" %*
if "%~1"=="" pause
