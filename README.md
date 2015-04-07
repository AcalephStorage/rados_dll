# rados_dll
Windows port of Ceph librados.

#### Pre-requisites
* mingw                 - [http://sourceforge.net/projects/mingw/files/Installer/mingw-get-setup.exe/download](http://sourceforge.net/projects/mingw/files/Installer/mingw-get-setup.exe/download)
* pthreads 2.9.1        - [http://sourceforge.net/projects/pthreads4w/files/latest/download](http://sourceforge.net/projects/pthreads4w/files/latest/download)
* boost 1.57.0          - [http://sourceforge.net/projects/boost/files/boost/1.57.0/boost_1_57_0.zip/download](http://sourceforge.net/projects/boost/files/boost/1.57.0/boost_1_57_0.zip/download)
* glib-2.0              - [http://win32builder.gnome.org/packages/3.6/glib-dev_2.34.3-1_win32.zip](http://win32builder.gnome.org/packages/3.6/glib-dev_2.34.3-1_win32.zip)

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

Copy or create `ceph.conf` under `bin` folder, then:

```
$ cd bin
$ rados_client.exe
```

Tested against Ceph v0.92
