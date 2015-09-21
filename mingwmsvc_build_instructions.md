# mingw-w64 / MSVC Build Instructions

## meshlink dependencies:
zlib (and crypto32?) included with mingw-w64 install<br/>
to build zlib on yourself (-fPIC required) however proceed with:
```
cd zlib-1.2.8
export "DESTDIR=/c/lib/zlib"
export "INCLUDE_PATH=/c/lib/zlib/include"
export "LIBRARY_PATH=/c/lib/zlib/lib"
export "BINARY_PATH=/c/lib/zlib/bin"
make -f win32/Makefile.gcc CFLAGS='-fPIC'
make install -f win32/Makefile.gcc
```


## autoreconf:
-f/--force   force to remake configure script<br/>
-s/--symlink install symbolic links to the missing auxiliary files instead of copying them<br/>
-i/--install install the missing auxiliary files in the package<br/>
-Wnone       no warnings
```
catta/autoreconf -fsiWnone
autoreconf -fsiWnone
```

## configure:
for debug add: '-g -O0'<br/>
-g for debug symbols<br/>
-O0 for optimization level 0<br/>
-fPIC to generate position-independent code and if supported avoid any limit on the size of the global offset table<br/>
-fstack-protector-all add guards to check for buffer overflows, protect all functions<br/>
-std=c99 use c99 standard<br/>
catta/configure CFLAGS='-fPIC -fstack-protector-all -std=c99' --prefix=<INSTALL_DIR><br/>
configure CFLAGS='-fPIC -fstack-protector-all -std=c99' --with-zlib-include=${ZLIB_INCLUDE_DIR} --with-zlib-lib=${ZLIB_LIBRARY_DIR} --prefix=<INSTALL_DIR><br/>
```
catta/configure CFLAGS='-fPIC -fstack-protector-all -std=c99' --prefix='/c/lib/catta'
configure CFLAGS='-fPIC -fstack-protector-all -std=c99' --prefix='/c/lib/meshlink' --with-zlib-lib=/c/lib/zlib/lib
```

## build:
```
catta/make
make
```


## install:
```
catta/make install
make install
```
just a note: libtool, used by the autotools, doesn't allow to link static libraries to dynamic ones<br/>
even when build with -fPIC and perfectly legal it insists to link dependencies dynamicly or aborts the build<br/>
to circumvent this, it now builds passing -lz in LDFLAGS<br/>
however another approach I found is to just make a static build and convert to dll + def afterwards:<br/>
``gcc -shared -Wl,--whole-archive,--kill-at,--output-def=libmeshlink-0.def libmeshlink.a -Wl,--no-whole-archive -L../../catta/src/.libs -L/c/lib/zlib/lib -lpthread -liphlpapi -lssp -lws2_32 -lgdi32 -lcatta.dll -lz -o libmeshlink-0.dll``


## generate msvc import library for meshlink + catta (using cmd shell)
```
vcvarsall amd64
cd meshlink/catta/src/.libs
lib /machine:x64 /def:libcatta-0.def
cd meshlink/src/.libs
lib /machine:x64 /def:libmeshlink-0.def
```


## MSVC project setup
copy libcatta-0.lib and libmeshlink-0.lib to lib install folders<br/>
for usage don't forget to copy gcc library dependencies to your exe path
```
libgcc_s_seh-1.dll
libssp-0.dll
libwinpthread-1.dll
```
Include Directories: ``C:\lib\catta\include\catta\compat\windows;C:\lib\catta\include;C:\lib\meshlink\include;``<br/>
Library Directories: ``C:\lib\catta\lib;C:\lib\meshlink\lib;``<br/>
Linker.Input Additional Dependencies: ``libmeshlink-0.lib;libcatta-0.lib``<br/>