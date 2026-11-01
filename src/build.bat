@echo off
setlocal
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VS_PATH=%%i"
)
call "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat"
cl.exe /nologo /std:c++17 /O2 /Oi /MT /EHsc /DUNICODE /D_UNICODE dllmain.cpp /link /NOLOGO /DLL /DEF:dxgi.def /MACHINE:X64 dxguid.lib user32.lib /OUT:dxgi.dll
endlocal
