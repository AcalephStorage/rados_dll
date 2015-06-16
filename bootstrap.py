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

DOWNLOADS_DIR = os.path.join(os.path.expanduser("~"), "Downloads")
MINGW_DIR = os.path.join("C:\\", "MinGw")

PATH_TO_7Z = os.path.join("C:\\", "Program Files", "7-Zip", "7z.exe")
SEVEN_ZIP_URL = "http://www.7-zip.org/a/7z1505-x64.exe"

PATH_TO_MINGW_GET = os.path.join(MINGW_DIR, "bin", "mingw-get.exe")

PATH_TO_MINGW_GET_SETUP = os.path.join(DOWNLOADS_DIR, "mingw-get-setup.exe")
MINGW_GET_SETUP_URL = "http://sourceforge.net/projects/mingw/files/Installer/mingw-get-setup.exe/download"

MINGW_GET_URL = "http://downloads.sourceforge.net/project/mingw/Installer/mingw-get/mingw-get-0.6.2-beta-20131004-1/mingw-get-0.6.2-mingw32-beta-20131004-1-bin.zip"

def check_or_download(url, filename = None, base_dir = DOWNLOADS_DIR):
	if not os.path.isdir(base_dir):
		os.makedirs(base_dir)
	target = filename if filename is not None else url.rsplit('/', 1)[1]
	path = os.path.join(base_dir, target)
	print "path: {}\n".format(path)
	if os.path.isfile(path):
		return path
	else:
		downloaded = wget.download(url, base_dir)
		if filename is None:
			return downloaded
		else:
			os.rename(downloaded, path)
			return path

def main(args):
	# Bail if we're not running under Windows
	if not (os.name == "nt" and platform.system() == "Windows"):
		print "This script was meant to run under Windows.\n"
		sys.exit(1)

	if not os.path.isfile(PATH_TO_MINGW_GET):
		print "\"{}\" not found. Will attempt to download and install\n".format(PATH_TO_MINGW_GET)

		if not os.path.isfile(PATH_TO_MINGW_GET_SETUP):
			print "\"{}\" not found. Will attempt to download and install\n".format(PATH_TO_MINGW_GET_SETUP)
			check_or_download(MINGW_GET_SETUP_URL, filename="mingw-get-setup.exe")

		subprocess.call(PATH_TO_MINGW_GET_SETUP, shell=True)

	subprocess.call([PATH_TO_MINGW_GET, "install", "base", "gcc", "g++", "mingw32-make", "pthreads-w32"])

	if not os.path.isfile(PATH_TO_7Z):
		print "\"{}\" not found. Will attempt to download and install\n".format(PATH_TO_7Z)
		seven_zip_filename = check_or_download(SEVEN_ZIP_URL)
		subprocess.call(seven_zip_filename, shell=True)

if __name__ == "__main__":
	sys.exit(main(sys.argv))