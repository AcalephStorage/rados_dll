#!/bin/bash
#
# Copyright (C) 2013 Cloudwatt <libre.licensing@cloudwatt.com>
#
# Author: Loic Dachary <loic@dachary.org>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU Library Public License as published by
# the Free Software Foundation; either version 2, or (at your option)
# any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Library Public License for more details.
#
MDS=1 MON=1 OSD=3 CEPH_START='mon osd mds' CEPH_PORT=7200 test/vstart_wrapper.sh \
    ../qa/workunits/cephtool/test.sh \
    --test-mds \
    --asok-does-not-need-root
