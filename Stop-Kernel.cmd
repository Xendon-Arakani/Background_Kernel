@echo off
setlocal
set "BK_CONTROLLER=%~dp0KeypadControl.exe"
set "BK_DESKTOP=%~dp0SceneKernel.exe"
set "BK_VISUALIZER=%~dp0AudioScene.exe"
if not exist "%BK_CONTROLLER%" set "BK_CONTROLLER=%~dp0build\Release\KeypadControl.exe"
if not exist "%BK_DESKTOP%" set "BK_DESKTOP=%~dp0build\Release\SceneKernel.exe"
if not exist "%BK_VISUALIZER%" set "BK_VISUALIZER=%~dp0build\Release\AudioScene.exe"
if not exist "%BK_DESKTOP%" (
    echo kernel_missing
    exit /b 2
)
if exist "%BK_CONTROLLER%" (
    "%BK_CONTROLLER%" --stop
    exit /b %ERRORLEVEL%
)
"%BK_DESKTOP%" --stop
if exist "%BK_VISUALIZER%" "%BK_VISUALIZER%" --stop
exit /b %ERRORLEVEL%
