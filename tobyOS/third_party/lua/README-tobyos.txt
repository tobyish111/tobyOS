Lua 5.4.7 (2024), vendored verbatim from
https://www.lua.org/ftp/lua-5.4.7.tar.gz
  SHA-256: 9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30

Lua is distributed under the MIT license (see the copyright notice at the top
of lua.h and https://www.lua.org/license.html). All of src/*.c and src/*.h here
are the UNMODIFIED upstream sources (the entire src/ directory of the tarball).
Built for tobyOS as a Windows PE (win-lua.exe) by the in-tree mingw clang to
prove Track C runs a real, off-the-shelf third-party language interpreter
(milestone C19b). luac.c (the standalone bytecode-compiler tool) is vendored for
completeness but is excluded from the win-lua.exe build, which compiles lua.c
(the interpreter front-end) plus the core + standard-library translation units.
No source modifications; the LUA_USE_WINDOWS platform profile is selected
automatically by luaconf.h on the _WIN32 mingw target.
