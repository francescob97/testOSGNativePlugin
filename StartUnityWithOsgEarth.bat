@echo off
rem ---------------------------------------------------------------------------
rem Starts the Unity Editor on UnityProject with Direct3D 11 and with the
rem OpenSceneGraph/osgEarth runtime reachable by RenderingPlugin.dll.
rem
rem   OSGEARTH_SDK : OSG + osgEarth install used to build the plugin
rem                  (e.g. C:\vcpkg\installed\x64-windows)
rem   UNITY_EDITOR : full path of Unity.exe (2022.3)
rem
rem Both can be set as environment variables or edited below.
rem ---------------------------------------------------------------------------
setlocal
if "%OSGEARTH_SDK%"=="" set "OSGEARTH_SDK=C:\vcpkg\installed\x64-windows"
if "%UNITY_EDITOR%"=="" set "UNITY_EDITOR=C:\Program Files\Unity\Hub\Editor\2022.3.62f1\Editor\Unity.exe"

if not exist "%OSGEARTH_SDK%\bin\" goto no_sdk
if not exist "%UNITY_EDITOR%" goto no_unity

rem DLLs loaded by RenderingPlugin.dll: osg*.dll, osgEarth.dll, gdal, proj, curl, ...
set "PATH=%OSGEARTH_SDK%\bin;%PATH%"
rem OSG plugins (osgPlugins-3.6.5\osgdb_earth.dll ...): vcpkg puts them in "plugins",
rem a CMake install of OSG in "bin"
set "OSG_LIBRARY_PATH=%OSGEARTH_SDK%\plugins;%OSGEARTH_SDK%\bin"
rem PROJ coordinate system database (proj.db) and GDAL data
if exist "%OSGEARTH_SDK%\share\proj\" set "PROJ_DATA=%OSGEARTH_SDK%\share\proj"
if exist "%OSGEARTH_SDK%\share\proj\" set "PROJ_LIB=%OSGEARTH_SDK%\share\proj"
if exist "%OSGEARTH_SDK%\share\gdal\" set "GDAL_DATA=%OSGEARTH_SDK%\share\gdal"

start "" "%UNITY_EDITOR%" -projectPath "%~dp0UnityProject" -force-d3d11
exit /b 0

:no_sdk
echo OSGEARTH_SDK not found: "%OSGEARTH_SDK%"
pause
exit /b 1

:no_unity
echo Unity.exe not found: "%UNITY_EDITOR%"
pause
exit /b 1
