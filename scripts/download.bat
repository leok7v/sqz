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

if not exist corpus (
    mkdir corpus 2>nul >nul
)

cd corpus

if not exist "dickens.txt" ( :: Collected works of Charles Dickens Project Gutenberg
    curl -LJO https://github.com/leok7v/SilesiaCorpus/raw/refs/heads/master/dickens
    rename dickens dickens.txt
)

if not exist "mozilla.tar" ( :: Tarred executables of Mozilla 1.0 (Tru64 UNIX edition)
    curl -LJO https://github.com/leok7v/SilesiaCorpus/raw/refs/heads/master/mozilla
    rename mozilla mozilla.tar
)

if not exist "mr.dicom" ( :: Medical magnetic resonanse image 3-D MRI image, DICOM
    curl -LJO https://github.com/leok7v/SilesiaCorpus/raw/refs/heads/master/mr
    rename mr mr.dicom
)

if not exist "nci.txt" (
    :: Chemical database of structures CACTVS Chemical Information Services at LMC/NCI
    curl -LJO https://github.com/leok7v/SilesiaCorpus/raw/refs/heads/master/nci
    rename nci nci.txt
)

if not exist "ooffice.dll" ( :: A dll from Open Office.org 1.01
    curl -LJO https://github.com/leok7v/SilesiaCorpus/raw/refs/heads/master/ooffice
    rename ooffice ooffice.dll
)

if not exist "os.db" (
    :: Sample database in MySQL format from Open Source Database Benchmark
    curl -LJO https://github.com/leok7v/SilesiaCorpus/raw/refs/heads/master/osdb
    rename osdb os.db
)

if not exist "reymont.pdf" (
    :: Polish text, uncompressed PDF Wladyslaw Reymont - Chłopi
    curl -LJO https://github.com/leok7v/SilesiaCorpus/raw/refs/heads/master/reymont
    rename reymont reymont.pdf
)

if not exist "samba.tar" ( :: Tarred source code of Samba 2-2.3
    curl -LJO https://github.com/leok7v/SilesiaCorpus/raw/refs/heads/master/samba
    rename samba samba.tar
)

if not exist "sao.bin" (
    :: The SAO star catalog Astronomical Catalogs and Catalog Formats
    curl -LJO https://github.com/leok7v/SilesiaCorpus/raw/refs/heads/master/sao
    rename sao sao.bin
)

if not exist "webster.html" (
    :: The 1913 Webster Unabridged Dictionary Project Gutenberg
    curl -LJO https://github.com/leok7v/SilesiaCorpus/raw/refs/heads/master/webster
    rename webster webster.html
)

if not exist "x-ray.dicom" ( :: X-ray medical picture 16 bit grayscale, DICOM
    curl -LJO https://github.com/leok7v/SilesiaCorpus/raw/refs/heads/master/x-ray
    rename x-ray x-ray.dicom
)

if not exist "xml.tar" (
    :: Collected XML files XMLPPM: XML-Conscious PPM Compression
    curl -LJO https://github.com/leok7v/SilesiaCorpus/raw/refs/heads/master/xml
    rename xml xml.tar
)

if not exist "xml.tar" (
    :: Collected XML files XMLPPM: XML-Conscious PPM Compression
    curl -LJO https://github.com/leok7v/SilesiaCorpus/raw/refs/heads/master/xml
    rename xml xml.tar
)

cd ..

:: https://github.com/inikep/lzbench
:: https://github.com/inikep/lzbench/blob/master/lzbench18_sorted.md

if not exist "silesia.tar" (
    powershell -Command "tar -cf silesia.tar corpus/"
)

popd

:: *) Disney, the majority owner of Hulu, owns the rights to Adams' novels.
:: Since this repository does not contain the text of the book and uses
:: it only if available for testing purposes and not derived work or
:: redistribution it may or may not be considered as fair use.
:: If it breaks copyright laws - it is on "Jose Ramon Aleman":
:: https://github.com/jraleman
