# Turns a binary file into a C++ source defining  extern const unsigned char NAME[]  and  extern const unsigned NAME_size.
# Run at build time:  cmake -DIN=<file> -DOUT=<file.cpp> -DNAME=<symbol> -P EmbedFile.cmake
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" len)
math(EXPR size "${len} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
# Break the initializer into lines of 32 bytes so compilers and editors stay happy.
string(REGEX REPLACE "((0x..,){32})" "\\1\n" bytes "${bytes}")
file(WRITE "${OUT}" "// Generated from ${IN} by cmake/EmbedFile.cmake; do not edit.\n"
  "extern const unsigned char ${NAME}[] = {\n${bytes}\n};\n"
  "extern const unsigned ${NAME}_size = ${size};\n")
