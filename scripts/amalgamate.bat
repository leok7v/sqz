::: amalgamate.bat
@echo off
if exist ..\shl (
    if not exist ..\shl\sqz (
        mkdir ..\shl\sqz
    )
    (
        type ..\inc\sqz\sqz.h
        ::: LF
        echo.
        echo #ifdef sqz_implementation
        type ..\src\sqz.c
        echo.
        echo #endif // sqz_implementation
        echo.
    ) > ..\shl\sqz\sqz.h
)
