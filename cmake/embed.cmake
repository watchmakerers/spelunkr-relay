# Turns a binary file into a C header: cmake -DIN=file -DOUT=header.h -DNAME=symbol -P embed.cmake
# (the PS3 build gets the same symbol from PSL1GHT's bin2o).
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" n)
math(EXPR size "${n} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," body "${hex}")
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){32})" "\\1\n" body "${body}")
file(WRITE "${OUT}" "// Generated from ${IN}\n#pragma once\nstatic const unsigned char ${NAME}[${size}] = {\n${body}\n};\nstatic const unsigned int ${NAME}_size = ${size};\n")
