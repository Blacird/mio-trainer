@echo off
REM Build MIO_Trainer.exe from source. Requires MinGW-w64 (gcc + windres) on PATH.
windres MIO_Trainer.rc -o MIO_Trainer.res.o
gcc -O2 -s -mwindows -o MIO_Trainer.exe MIO_Trainer.c MIO_Trainer.res.o -lpsapi -lgdi32 -luser32
if %errorlevel%==0 ( echo Build OK: MIO_Trainer.exe ) else ( echo Build FAILED )
