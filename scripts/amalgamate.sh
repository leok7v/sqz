#!/bin/sh
mkdir -p ../shl/sqz
cat ../inc/sqz/sqz.h > ../shl/sqz/sqz.h
echo '' >> ../shl/sqz/sqz.h
echo '#ifdef sqz_implementation' >> ../shl/sqz/sqz.h
cat ../src/sqz.c >> ../shl/sqz/sqz.h
echo '' >> ../shl/sqz/sqz.h
echo '#endif // sqz_implementation' >> ../shl/sqz/sqz.h
echo '' >> ../shl/sqz/sqz.h
