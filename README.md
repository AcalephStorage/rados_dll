# rados_dll
Windows port of Ceph librados.

#### Pre-requisites
* mingw                 - [http://sourceforge.net/projects/mingw/files/Installer/mingw-get-setup.exe/download](http://sourceforge.net/projects/mingw/files/Installer/mingw-get-setup.exe/download)
* pthreads 2.9.1        - [http://sourceforge.net/projects/pthreads4w/files/latest/download](http://sourceforge.net/projects/pthreads4w/files/latest/download)
* boost 1.57.0          - [http://sourceforge.net/projects/boost/files/boost/1.57.0/boost_1_57_0.zip/download](http://sourceforge.net/projects/boost/files/boost/1.57.0/boost_1_57_0.zip/download)
* glib-2.0              - [http://win32builder.gnome.org/packages/3.6/glib-dev_2.34.3-1_win32.zip](http://win32builder.gnome.org/packages/3.6/glib-dev_2.34.3-1_win32.zip)

#### Preparation

```
C:\rados_dll>python bootstrap.py
```

Run it again after checking/modifying your PATH

```
C:\rados_dll>python bootstrap.py
```

Install Microsoft Visual Studio 2013

#### Build NSS

```
C:\mozilla-build\start-shell-msvc2013.bat

$ cd Downloads/nss-3.18/nss

~/Downloads/nss-3.18/nss
$ make nss_build_all
```




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
