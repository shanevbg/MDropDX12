set releasePath=..\Release
set backupPath=..\Release\backup
set visualizerBuildPath=..\src\mDropDX12\Release

copy %releasePath%\settings.ini %backupPath%\settings.ini.bak

del /q %releasePath%\capture\*.*
del /q %releasePath%\resources\buttons\btn-0*.png
del /q %releasePath%\resources\buttons\btn-1*.png
del /q %releasePath%\resources\buttons\btn-2*.png
del /q %releasePath%\resources\buttons\btn-3*.png
del /q %releasePath%\resources\buttons\btn-4*.png

copy *.ini %releasePath%
copy *.txt %releasePath%
copy %visualizerBuildPath%\MDropDX12.exe %releasePath%

copy ..\resources\sprites\cover.png %releasePath%\resources\sprites\cover.png

pause
