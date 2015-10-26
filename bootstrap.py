#!/usr/bin/env python
# -*- coding: utf-8 -*-
# rados_dll environment setup Python script
#
# Copyright (c) 2015 by Acaleph Pty.

import os
import platform
import subprocess
import sys
import wget

from os import path

CWD = os.getcwd()

# Bail if we're not running under Windows
if not (os.name == "nt" and platform.system() == "Windows"):
	print "This script was meant to run under Windows."
	sys.exit(1)

from _winreg import *

DOWNLOADS_DIR = path.join(path.expanduser("~"), "Downloads")

def check_or_download(url, filename = None, basedir = DOWNLOADS_DIR):
	if not path.isdir(basedir):
		os.makedirs(basedir)
	os.chdir(basedir)
	target = path.join(basedir, filename if filename is not None else url.rsplit('/', 1)[1])
	print "Checking {}".format(target)
	if path.isfile(target):
		return target
	print "Downloading {}".format(target)
	return wget.download(url, target)

MINGW_DIR = path.join("C:\\", "MinGw")

PATH_TO_MINGW_BIN = path.join(MINGW_DIR, "bin")
PATH_TO_MINGW_GET = path.join(PATH_TO_MINGW_BIN, "mingw-get.exe")

PATH_TO_MINGW_GET_SETUP = path.join(DOWNLOADS_DIR, "mingw-get-setup.exe")
MINGW_GET_SETUP_URL = "http://sourceforge.net/projects/mingw/files/Installer/mingw-get-setup.exe/download"

MINGW_GET_URL = "http://downloads.sourceforge.net/project/mingw/Installer/mingw-get/mingw-get-0.6.2-beta-20131004-1/mingw-get-0.6.2-mingw32-beta-20131004-1-bin.zip"

NSS_VER = "3.19.1"
NSS_VER_UNDERSCORE = NSS_VER.replace('.', '_')
PATH_TO_NSS = path.join(DOWNLOADS_DIR, "nss-" + NSS_VER)

def install_mingw_setup():
	if not path.isfile(PATH_TO_MINGW_GET_SETUP):
		check_or_download(MINGW_GET_SETUP_URL, filename="mingw-get-setup.exe")
		print
	subprocess.call(PATH_TO_MINGW_GET_SETUP, shell=True)

def install_mingw():
	if not path.isfile(PATH_TO_MINGW_GET):
		print "\"{}\" not found. Will attempt to download and install".format(PATH_TO_MINGW_GET)
		install_mingw_setup()
	subprocess.call([PATH_TO_MINGW_GET, "install", "base", "gcc", "g++", "msys-make", "pthreads-w32"])

def get_user_path(aReg):
	aKey = OpenKey(aReg, r"Environment")
	try:
		for i in range(1024):
		    try:
		        n, v, t = EnumValue(aKey, i)
		        if n == "Path":
		        	return v
		    except WindowsError:
		        break
	finally:
		CloseKey(aKey)

def set_user_path(aReg, new_user_path):
	aKey = OpenKey(aReg, "Environment", 0, KEY_WRITE)
	try:
	   SetValueEx(aKey,"Path",0, REG_SZ, new_user_path)
	   print "Please close this terminal and rerun \"python bootstrap.py\""
	except EnvironmentError:
	    print "Encountered problems writing into the Registry..."
	finally:
		CloseKey(aKey)

def add_to_user_path(path):
	print "\"{}\" not in path! Will attempt to set registry.".format(path)
	aReg = ConnectRegistry(None,HKEY_CURRENT_USER)
	try:
		current_user_path = get_user_path(aReg)
		print "current_user_path: ", current_user_path
		if current_user_path is None or path not in current_user_path.split(';'):
			new_user_path = path if current_user_path is None else current_user_path + ";" + path
			print r"*** Writing new %PATH% registry entry to user environment ***"
			set_user_path(aReg, new_user_path)
		else:
		   print "\"{}\" already in registry. Please close this terminal and rerun \"python bootstrap.py\"".format(PATH_TO_MINGW_BIN)
	finally:
		CloseKey(aReg)

def ensure_all_in_path(paths):
	current_env_path = os.environ["PATH"].split(';')
	if all(path in current_env_path for path in paths):
		return True
	for path in paths:
		if not path in current_env_path:
			add_to_user_path(path)
	return False

PATH_TO_MSYS_BIN = path.join(MINGW_DIR, "msys", "1.0", "bin")

def modify_user_path():
	if not ensure_all_in_path([PATH_TO_MINGW_BIN, PATH_TO_MSYS_BIN, path.join(PATH_TO_NSS, "dist", "WIN954.0_DBG.OBJ", "lib")]):
		sys.exit(1)

PATH_TO_7Z = path.join("C:\\", "Program Files", "7-Zip", "7z.exe")
SEVEN_ZIP_URL = "http://www.7-zip.org/a/7z1505-x64.exe"

def install_7za():
	if not path.isfile(PATH_TO_7Z):
		print "\n\"{}\" not found. Will attempt to download and install".format(PATH_TO_7Z)
		seven_zip_filename = check_or_download(SEVEN_ZIP_URL)
		subprocess.call(seven_zip_filename, shell=True)

BOOST_VER = "1.58.0"
BOOST_VER_UNDERSCORE = BOOST_VER.replace('.', '_')
BOOST_DIR = path.join(DOWNLOADS_DIR, "boost_" + BOOST_VER_UNDERSCORE)
BOOST_URL = "http://sourceforge.net/projects/boost/files/boost/" + BOOST_VER + "/boost_" + BOOST_VER_UNDERSCORE + ".zip/download"

def install_and_compile_boost():
	if not path.isfile(path.join(BOOST_DIR, "bootstrap.bat")):
		print "\n Will download and compile boost libraries."
		boost_zip_filename = check_or_download(BOOST_URL, filename="boost_" + BOOST_VER_UNDERSCORE + ".zip")
		subprocess.call([PATH_TO_7Z, "x", "-y", boost_zip_filename])
	os.chdir(BOOST_DIR)
	if not path.isdir(path.join(BOOST_DIR, "bin.v2")):
		if not path.isfile(path.join(BOOST_DIR, "b2.exe")):
			subprocess.call([path.join(BOOST_DIR, "bootstrap.bat"), "mingw"])
		subprocess.call([path.join(BOOST_DIR, "b2.exe"), "toolset=gcc", "variant=release"])

PATH_TO_GLIB = path.join(DOWNLOADS_DIR, "glib-dev_2.34.3-1_win32")
GLIB_URL = "http://win32builder.gnome.org/packages/3.6/glib-dev_2.34.3-1_win32.zip"

def download_and_extract_glib():
	if not path.isdir(path.join(PATH_TO_GLIB)):
		glib_zip_filename = check_or_download(GLIB_URL)
		print "Extracting glib"
		os.makedirs(PATH_TO_GLIB)
		os.chdir(PATH_TO_GLIB)
		subprocess.call([PATH_TO_7Z, "x", "-y", glib_zip_filename])

MOZILLA_BUILD_URL = "http://ftp.mozilla.org/pub/mozilla.org/mozilla/libraries/win32/MozillaBuildSetup-Latest.exe"
PATH_TO_MOZILLA_BUILD = path.join(DOWNLOADS_DIR, "")

def install_mozilla_build():
	if not (path.isdir("C:\\mozilla-build") and path.isfile("C:\\mozilla-build\\start-shell-msvc2013.bat")):
		print "Downloading and installing Mozilla build"
		mozilla_build_filename = check_or_download(MOZILLA_BUILD_URL)
		subprocess.call(mozilla_build_filename, shell=True)

NSS_FTP_URL = "ftp://ftp.mozilla.org/pub/mozilla.org/security/nss/releases/NSS_" + NSS_VER_UNDERSCORE + "_RTM/src/nss-" + NSS_VER + "-with-nspr-4.10.8.tar.gz"

def download_nss():
	if not path.isdir(PATH_TO_NSS):
		print
		nss_filename = check_or_download(NSS_FTP_URL)
		print
		print "Extracting {}".format(NSS_FTP_URL.rsplit('/',1)[1])
		subprocess.call([PATH_TO_7Z, "x", "-y", nss_filename, "-so", "|", PATH_TO_7Z, "x", "-aoa", "-si","-ttar"], shell=True)

def main(args):
	# Make sure the ~/Downloads directory exists, and chdir to that
	if not path.isdir(DOWNLOADS_DIR):
		os.makedirs(DOWNLOADS_DIR)
	os.chdir(DOWNLOADS_DIR)

	install_mingw()

	modify_user_path()

	install_7za()
	install_and_compile_boost()

	download_and_extract_glib()

	install_mozilla_build()
	download_nss()

if __name__ == "__main__":
	sys.exit(main(sys.argv))