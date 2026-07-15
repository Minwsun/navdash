@echo off
cd /d "%~dp0.."
python -u tools\capture_com.py COM4 captures\com4.log
