@echo off
rem Agent Buddy tool: bridge\buddy status ^| start ^| stop ^| restart ^| logs ^| flash ^| autostart on^|off
"%~dp0.venv\Scripts\python.exe" "%~dp0buddy.py" %*
