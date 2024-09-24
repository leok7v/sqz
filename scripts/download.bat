@echo off
if not exist ..\test (
    mkdir ..\test 2>nul >nul
)
pushd ..\test
:: Copyrighted *) "The Hitch Hiker's Guide to the Galaxy" by Douglas Adams
if not exist "hhgttg.txt" ( :: modern prose text file
    curl -LJO https://raw.githubusercontent.com/jraleman/42_get_next_line/master/tests/hhgttg.txt
)
if not exist "sqlite3.c" ( :: public domain amalgamated C source code
    curl -LJO https://raw.githubusercontent.com/jmscreation/libsqlite3/main/src/sqlite3.c
)
if not exist "mandrill.png" ( :: mandrill image USC SIPI Image Database
    curl -LJO https://upload.wikimedia.org/wikipedia/commons/c/c1/Wikipedia-sipi-image-db-mandrill-4.2.03.png
    rename Wikipedia-sipi-image-db-mandrill-4.2.03.png mandrill.png
)
if not exist "arm64.elf" ( :: arm64 executable sample
    curl -LJO https://github.com/JonathanSalwan/binary-samples/raw/master/elf-Linux-ARM64-bash
    rename elf-Linux-ARM64-bash arm64.elf
)
if not exist "x64.elf" ( :: x64 executable sample
    curl -LJO https://github.com/JonathanSalwan/binary-samples/raw/master/elf-Linux-x64-bash
    rename elf-Linux-x64-bash x64.elf
)
:: Gutenberg text files:
:: bible.txt without Gutenberg license noise
if not exist "kjv-bible.txt" ( :: public domain gutenberg KJV Bible
    curl -LJO https://www.gutenberg.org/cache/epub/10/pg10.txt
    rename pg10.txt kjv-bible.txt
)
:: laozi.txt without Gutenberg license noise
if not exist "lao-tzu.txt" ( :: "Tao Teh King" / "Dao De Jing" by Laozi / Lao Tzu
    curl -LJO https://www.gutenberg.org/cache/epub/24039/pg24039.txt
    rename pg24039.txt lao-tzu.txt
)
:: confucius.txt without Gutenberg license noise
if not exist "the-analects-of-confucius.txt" ( :: "The Analects" by Confucius
    curl -LJO https://www.gutenberg.org/cache/epub/23839/pg23839.txt
    rename pg23839.txt the-analects-of-confucius.txt
)
popd

:: *) Disney, the majority owner of Hulu, owns the rights to Adams' novels.
:: Since this repository does not contain the text of the book and uses
:: it only if available for testing purposes and not derived work or
:: redistribution it may or may not be considered as fair use.
:: If it breaks copyright laws - it is on "Jose Ramon Aleman":
:: https://github.com/jraleman
