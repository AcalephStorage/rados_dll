# rados_dll
Windows port of Ceph librados.

#### Pre-requisites
* mingw
* pthreads 2.9.1
* boost 1.57.0
* glib-2.0

#### Preparation
1) Install `mingw`

2) Copy `pthreads-win32\prebuilt-dll-2-9-1-release\lib\x64\libpthreadGC2.a` to `mingw\lib`

3) Compile `boost`

```
bootstrap.bat mingw
b2 toolset=gcc
```

4) Copy boost libraries to `mingw\lib`

5) Copy glib header files to `mingw\include`

6) Copy glib libraries to `mingw\lib`


#### Building

```
$ cd src
$ make
```

#### Testing

```
$ cd bin
$ rados_client.exe
```

Tested against Ceph v0.92
