@echo off
cd /d "%~dp0"
echo Starting... > run.log
"FrightWorks-No100F-Game.exe" >> run.log 2>&1
echo exit code %errorlevel% >> run.log
type run.log
pause
