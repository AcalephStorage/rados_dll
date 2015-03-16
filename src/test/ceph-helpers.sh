#!/bin/bash
#
# Copyright (C) 2013,2014 Cloudwatt <libre.licensing@cloudwatt.com>
# Copyright (C) 2014 Red Hat <contact@redhat.com>
# Copyright (C) 2014 Federico Gimenez <fgimenez@coit.es>
#
# Author: Loic Dachary <loic@dachary.org>
# Author: Federico Gimenez <fgimenez@coit.es>
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
CEPH_HELPER_VERBOSE=false
TIMEOUT=60
PG_NUM=4

if type xmlstarlet > /dev/null 2>&1; then
    XMLSTARLET=xmlstarlet
elif type xml > /dev/null 2>&1; then
    XMLSTARLET=xml
else
	echo "Missing xmlstarlet binary!"
	exit 1
fi

#! @file ceph-helpers.sh
#  @brief Toolbox to manage Ceph cluster dedicated to testing
#
#  Example use case:
#
#  ~~~~~~~~~~~~~~~~{.sh}
#  source ceph-helpers.sh
#
#  function mytest() {
#    # cleanup leftovers and reset mydir
#    setup mydir
#    # create a cluster with one monitor and three osds
#    run_mon mydir a
#    run_osd mydir 0
#    run_osd mydir 2
#    run_osd mydir 3
#    # put and get an object
#    rados --pool rbd put GROUP /etc/group
#    rados --pool rbd get GROUP /tmp/GROUP
#    # stop the cluster and cleanup the directory
#    teardown mydir
#  }
#  ~~~~~~~~~~~~~~~~
#
#  The focus is on simplicity and efficiency, in the context of
#  functional tests. The output is intentionally very verbose
#  and functions return as soon as an error is found. The caller
#  is also expected to abort on the first error so that debugging
#  can be done by looking at the end of the output.
#
#  Each function is documented, implemented and tested independently.
#  When modifying a helper, the test and the documentation are
#  expected to be updated and it is easier of they are collocated. A
#  test for a given function can be run with
#
#  ~~~~~~~~~~~~~~~~{.sh}
#    ceph-helpers.sh TESTS test_get_osds
#  ~~~~~~~~~~~~~~~~
#
#  and all the tests (i.e. all functions matching test_*) are run
#  with:
#
#  ~~~~~~~~~~~~~~~~{.sh}
#    ceph-helpers.sh TESTS
#  ~~~~~~~~~~~~~~~~
#
#  A test function takes a single argument : the directory dedicated
#  to the tests. It is expected to not create any file outside of this
#  directory and remove it entirely when it completes successfully.
#


##
# Cleanup any leftovers found in **dir** via **teardown**
# and reset **dir** as an empty environment.
#
# @param dir path name of the environment
# @return 0 on success, 1 on error
#
function setup() {
    local dir=$1
    teardown $dir || return 1
    mkdir -p $dir
}

function test_setup() {
    local dir=$dir
    setup $dir || return 1
    test -d $dir || return 1
    setup $dir || return 1
    test -d $dir || return 1
    teardown $dir
}

#######################################################################

##
# Kill all daemons for which a .pid file exists in **dir** and remove
# **dir**. If the file system in which **dir** is btrfs, delete all
# subvolumes that relate to it.
#
# @param dir path name of the environment
# @return 0 on success, 1 on error
#
function teardown() {
    local dir=$1
    kill_daemons $dir
    if [ $(stat -f -c '%T' .) == "btrfs" ]; then
        __teardown_btrfs $dir
    fi
    rm -fr $dir
}

function __teardown_btrfs() {
    local btrfs_base_dir=$1

    btrfs_dirs=`ls -l $btrfs_base_dir | egrep '^d' | awk '{print $9}'`
    for btrfs_dir in $btrfs_dirs
    do
        btrfs_subdirs=`ls -l $btrfs_base_dir/$btrfs_dir | egrep '^d' | awk '{print $9}'`
        for btrfs_subdir in $btrfs_subdirs
        do
            btrfs subvolume delete $btrfs_base_dir/$btrfs_dir/$btrfs_subdir
        done
    done
}

function test_teardown() {
    local dir=$dir
    setup $dir || return 1
    teardown $dir || return 1
    ! test -d $dir || return 1
}

#######################################################################

##
# Kill all daemons for which a .pid file exists in **dir**.  Each
# daemon is sent a **signal** and kill_daemons waits for it to exit
# during a few seconds. By default all daemons are killed. If a
# **name_prefix** is provided, only the daemons matching it are
# killed.
#
# Send KILL to all daemons : kill_daemons $dir
# Send TERM to all daemons : kill_daemons $dir TERM
# Send TERM to all osds : kill_daemons $dir TERM osd
#
# If a daemon is sent the TERM signal and does not terminate
# within a few seconds, it will still be running even after
# kill_daemons returns.
#
# @param dir path name of the environment
# @param signal name of the first signal (defaults to KILL)
# @param name_prefix only kill match daemons (defaults to all)
# @return 0 on success, 1 on error
#
function kill_daemons() {
    local dir=$1
    local signal=${2:-KILL}
    local name_prefix=$3 # optional, osd, mon, osd.1

    for pidfile in $(find $dir | grep $name_prefix'[^/]*\.pid') ; do
        pid=$(cat $pidfile)
        local send_signal=$signal
        for try in 0 1 1 1 2 3 ; do
            kill -$send_signal $pid 2> /dev/null || break
            send_signal=0
            sleep $try
        done
    done
}

function test_kill_daemons() {
    local dir=$1
    setup $dir || return 1
    run_mon $dir a --osd_pool_default_size=1 || return 1
    run_osd $dir 0 || return 1
    ceph osd dump | grep "osd.0 up" || return 1
    kill_daemons $dir TERM osd
    ceph osd dump | grep "osd.0 down" || return 1
    kill_daemons $dir TERM
    ! ceph --connect-timeout 1 status || return 1
    teardown $dir || return 1
}

#######################################################################

##
# Run a monitor by the name mon.**id** with data in **dir**/**id**.
# The logs can be found in **dir**/mon.**id**.log and the pid file
# is **dir**/mon.**id**.pid and the admin socket is
# **dir**/**id**/ceph-mon.**id**.asok.
#
# The remaining arguments are passed verbatim to ceph-mon --mkfs
# and the ceph-mon daemon.
#
# Two mandatory arguments must be provided: --fsid and --mon-host
# Instead of adding them to every call to run_mon, they can be
# set in the CEPH_ARGS environment variable to be read implicitly
# by every ceph command.
#
# The CEPH_CONF variable is expected to be set to /dev/null to
# only rely on arguments for configuration.
#
# Examples:
#
# CEPH_ARGS="--fsid=$(uuidgen) "
# CEPH_ARGS+="--mon-host=127.0.0.1:7018 "
# run_mon $dir a # spawn a mon and bind port 7018
# run_mon $dir a --debug-filestore=20 # spawn with filestore debugging
#
# The default rbd pool is deleted and replaced with a replicated pool
# with less placement groups to speed up initialization.
#
# A **dir**/ceph.conf file is created but not meant to be used by any
# function.  It is convenient for debugging a failure with:
#
#     ceph --conf **dir**/ceph.conf -s
#
# @param dir path name of the environment
# @param id mon identifier
# @param ... can be any option valid for ceph-mon
# @return 0 on success, 1 on error
#
function run_mon() {
    local dir=$1
    shift
    local id=$1
    shift
    local data=$dir

    ceph-mon \
        --id $id \
        --mkfs \
        --mon-data=$data \
        --run-dir=$dir \
        "$@" || return 1

    ceph-mon \
        --id $id \
        --mon-osd-full-ratio=.99 \
        --mon-data-avail-crit=1 \
        --paxos-propose-interval=0.1 \
        --osd-crush-chooseleaf-type=0 \
        --osd-pool-default-erasure-code-directory=.libs \
        --debug-mon 20 \
        --debug-ms 20 \
        --debug-paxos 20 \
        --chdir= \
        --mon-data=$data \
        --log-file=$dir/\$name.log \
        --mon-cluster-log-file=$dir/log \
        --run-dir=$dir \
        --pid-file=$dir/\$name.pid \
        "$@" || return 1

    cat > $dir/ceph.conf <<EOF
[global]
fsid = $(get_config mon a fsid)
mon host = $(get_config mon a mon_host)
EOF

    ceph osd pool delete rbd rbd --yes-i-really-really-mean-it || return 1
    ceph osd pool create rbd $PG_NUM || return 1
}

function test_run_mon() {
    local dir=$1

    setup $dir || return 1

    run_mon $dir a || return 1
    local size=$(CEPH_ARGS='' ceph --format=json daemon $dir/ceph-mon.a.asok \
        config get osd_pool_default_size)
    test "$size" = '{"osd_pool_default_size":"3"}' || return 1

    ! CEPH_ARGS='' ceph status || return 1
    CEPH_ARGS='' ceph --conf $dir/ceph.conf status || return 1

    kill_daemons $dir

    run_mon $dir a --osd_pool_default_size=1 || return 1
    local size=$(CEPH_ARGS='' ceph --format=json daemon $dir/ceph-mon.a.asok \
        config get osd_pool_default_size)
    test "$size" = '{"osd_pool_default_size":"1"}' || return 1
    kill_daemons $dir

    CEPH_ARGS="$CEPH_ARGS --osd_pool_default_size=2" \
        run_mon $dir a || return 1
    local size=$(CEPH_ARGS='' ceph --format=json daemon $dir/ceph-mon.a.asok \
        config get osd_pool_default_size)
    test "$size" = '{"osd_pool_default_size":"2"}' || return 1
    kill_daemons $dir

    teardown $dir || return 1
}

#######################################################################

##
# Create (prepare) and run (activate) an osd by the name osd.**id**
# with data in **dir**/**id**.  The logs can be found in
# **dir**/osd.**id**.log, the pid file is **dir**/osd.**id**.pid and
# the admin socket is **dir**/**id**/ceph-osd.**id**.asok.
#
# The remaining arguments are passed verbatim to ceph-osd.
#
# Two mandatory arguments must be provided: --fsid and --mon-host
# Instead of adding them to every call to run_osd, they can be
# set in the CEPH_ARGS environment variable to be read implicitly
# by every ceph command.
#
# The CEPH_CONF variable is expected to be set to /dev/null to
# only rely on arguments for configuration.
#
# The run_osd function creates the OSD data directory with ceph-disk
# prepare on the **dir**/**id** directory and relies on the
# activate_osd function to run the daemon.
#
# Examples:
#
# CEPH_ARGS="--fsid=$(uuidgen) "
# CEPH_ARGS+="--mon-host=127.0.0.1:7018 "
# run_osd $dir 0 # prepare and activate an osd using the monitor listening on 7018
#
# @param dir path name of the environment
# @param id osd identifier
# @param ... can be any option valid for ceph-osd
# @return 0 on success, 1 on error
#
function run_osd() {
    local dir=$1
    shift
    local id=$1
    shift
    local osd_data=$dir/$id

    local ceph_disk_args
    ceph_disk_args+=" --statedir=$dir"
    ceph_disk_args+=" --sysconfdir=$dir"
    ceph_disk_args+=" --prepend-to-path="
    $CEPH_HELPER_VERBOSE && ceph_disk_args+=" --verbose"

    mkdir -p $osd_data
    ceph-disk $ceph_disk_args \
        prepare $osd_data || return 1

    activate_osd $dir $id "$@"
}

function test_run_osd() {
    local dir=$1

    setup $dir || return 1

    run_mon $dir a || return 1

    run_osd $dir 0 || return 1
    local backfills=$(CEPH_ARGS='' ceph --format=json daemon $dir//ceph-osd.0.asok \
        config get osd_max_backfills)
    test "$backfills" = '{"osd_max_backfills":"10"}' || return 1

    run_osd $dir 1 --osd-max-backfills 20 || return 1
    local backfills=$(CEPH_ARGS='' ceph --format=json daemon $dir//ceph-osd.1.asok \
        config get osd_max_backfills)
    test "$backfills" = '{"osd_max_backfills":"20"}' || return 1

    CEPH_ARGS="$CEPH_ARGS --osd-max-backfills 30" run_osd $dir 2 || return 1
    local backfills=$(CEPH_ARGS='' ceph --format=json daemon $dir//ceph-osd.2.asok \
        config get osd_max_backfills)
    test "$backfills" = '{"osd_max_backfills":"30"}' || return 1

    teardown $dir || return 1
}

#######################################################################

##
# Run (activate) an osd by the name osd.**id** with data in
# **dir**/**id**.  The logs can be found in **dir**/osd.**id**.log,
# the pid file is **dir**/osd.**id**.pid and the admin socket is
# **dir**/**id**/ceph-osd.**id**.asok.
#
# The remaining arguments are passed verbatim to ceph-osd.
#
# Two mandatory arguments must be provided: --fsid and --mon-host
# Instead of adding them to every call to activate_osd, they can be
# set in the CEPH_ARGS environment variable to be read implicitly
# by every ceph command.
#
# The CEPH_CONF variable is expected to be set to /dev/null to
# only rely on arguments for configuration.
#
# The activate_osd function expects a valid OSD data directory
# in **dir**/**id**, either just created via run_osd or re-using
# one left by a previous run of ceph-osd. The ceph-osd daemon is
# run indirectly via ceph-disk activate.
#
# The activate_osd function blocks until the monitor reports the osd
# up. If it fails to do so within $TIMEOUT seconds, activate_osd
# fails.
#
# Examples:
#
# CEPH_ARGS="--fsid=$(uuidgen) "
# CEPH_ARGS+="--mon-host=127.0.0.1:7018 "
# activate_osd $dir 0 # activate an osd using the monitor listening on 7018
#
# @param dir path name of the environment
# @param id osd identifier
# @param ... can be any option valid for ceph-osd
# @return 0 on success, 1 on error
#
function activate_osd() {
    local dir=$1
    shift
    local id=$1
    shift
    local osd_data=$dir/$id

    local ceph_disk_args
    ceph_disk_args+=" --statedir=$dir"
    ceph_disk_args+=" --sysconfdir=$dir"
    ceph_disk_args+=" --prepend-to-path="
    $CEPH_HELPER_VERBOSE && ceph_disk_args+=" --verbose"

    local ceph_args="$CEPH_ARGS"
    ceph_args+=" --osd-backfill-full-ratio=.99"
    ceph_args+=" --osd-failsafe-full-ratio=.99"
    ceph_args+=" --osd-journal-size=100"
    ceph_args+=" --osd-data=$osd_data"
    ceph_args+=" --chdir="
    ceph_args+=" --osd-pool-default-erasure-code-directory=.libs"
    ceph_args+=" --run-dir=$dir"
    ceph_args+=" --debug-osd=20"
    ceph_args+=" --log-file=$dir/\$name.log"
    ceph_args+=" --pid-file=$dir/\$name.pid"
    ceph_args+=" "
    ceph_args+="$@"
    mkdir -p $osd_data
    CEPH_ARGS="$ceph_args " ceph-disk $ceph_disk_args \
        activate \
        --mark-init=none \
        $osd_data || return 1

    [ "$id" = "$(cat $osd_data/whoami)" ] || return 1

    ceph osd crush create-or-move "$id" 1 root=default host=localhost

    status=1
    for ((i=0; i < $TIMEOUT; i++)); do
        if ! ceph osd dump | grep "osd.$id up"; then
            sleep 1
        else
            status=0
            break
        fi
    done

    return $status
}

function test_activate_osd() {
    local dir=$1

    setup $dir || return 1

    run_mon $dir a || return 1

    run_osd $dir 0 || return 1
    local backfills=$(CEPH_ARGS='' ceph --format=json daemon $dir//ceph-osd.0.asok \
        config get osd_max_backfills)
    test "$backfills" = '{"osd_max_backfills":"10"}' || return 1

    kill_daemons $dir TERM osd

    activate_osd $dir 0 --osd-max-backfills 20 || return 1
    local backfills=$(CEPH_ARGS='' ceph --format=json daemon $dir//ceph-osd.0.asok \
        config get osd_max_backfills)
    test "$backfills" = '{"osd_max_backfills":"20"}' || return 1

    teardown $dir || return 1
}

#######################################################################

##
# Display the list of OSD ids supporting the **objectname** stored in
# **poolname**, as reported by ceph osd map.
#
# @param poolname an existing pool
# @param objectname an objectname (may or may not exist)
# @param STDOUT white space separated list of OSD ids
# @return 0 on success, 1 on error
#
function get_osds() {
    local poolname=$1
    local objectname=$2

    ceph --format xml osd map $poolname $objectname 2>/dev/null | \
        $XMLSTARLET sel -t -m "//acting/osd" -v . -o ' '
}

function test_get_osds() {
    local dir=$1

    setup $dir || return 1
    run_mon $dir a --osd_pool_default_size=2 || return 1
    run_osd $dir 0 || return 1
    run_osd $dir 1 || return 1
    wait_for_clean || return 1
    get_osds rbd GROUP | grep --quiet '[0-1] [0-1] ' || return 1
    teardown $dir || return 1
}

#######################################################################

##
# Return the PG of supporting the **objectname** stored in
# **poolname**, as reported by ceph osd map.
#
# @param poolname an existing pool
# @param objectname an objectname (may or may not exist)
# @param STDOUT a PG
# @return 0 on success, 1 on error
#
function get_pg() {
    local poolname=$1
    local objectname=$2

    ceph --format xml osd map $poolname $objectname 2>/dev/null | \
        $XMLSTARLET sel -t -m "//pgid" -v . -n
}

function test_get_pg() {
    local dir=$1

    setup $dir || return 1
    run_mon $dir a --osd_pool_default_size=1 || return 1
    run_osd $dir 0 || return 1
    wait_for_clean || return 1
    get_pg rbd GROUP | grep --quiet '^[0-9]\.[0-9a-f][0-9a-f]*$' || return 1
    teardown $dir || return 1
}

#######################################################################

##
# Return the value of the **config**, obtained via the config get command
# of the admin socket of **daemon**.**id**.
#
# @param daemon mon or osd
# @param id mon or osd ID
# @param config the configuration variable name as found in config_opts.h
# @param STDOUT the config value
# @return 0 on success, 1 on error
#
function get_config() {
    local daemon=$1
    local id=$2
    local config=$3

    CEPH_ARGS='' \
        ceph --format xml daemon $dir/ceph-mon.$id.asok \
        config get $config 2> /dev/null | \
        $XMLSTARLET sel -t -m "//$config" -v . -n
}

function test_get_config() {
    local dir=$1

    setup $dir || return 1
    run_mon $dir a --osd_pool_default_size=1 || return 1
    test $(get_config mon a ms_crc_header) = true || return 1
    teardown $dir || return 1
}

#######################################################################

##
# Return the OSD id of the primary OSD supporting the **objectname**
# stored in **poolname**, as reported by ceph osd map.
#
# @param poolname an existing pool
# @param objectname an objectname (may or may not exist)
# @param STDOUT the primary OSD id
# @return 0 on success, 1 on error
#
function get_primary() {
    local poolname=$1
    local objectname=$2

    ceph --format xml osd map $poolname $objectname 2>/dev/null | \
        $XMLSTARLET sel -t -m "//acting_primary" -v . -n
}

function test_get_primary() {
    local dir=$1

    setup $dir || return 1
    run_mon $dir a --osd_pool_default_size=1 || return 1
    local osd=0
    run_osd $dir $osd || return 1
    wait_for_clean || return 1
    test $(get_primary rbd GROUP) = $osd || return 1
    teardown $dir || return 1
}

#######################################################################

##
# Return the id of any OSD supporting the **objectname** stored in
# **poolname**, as reported by ceph osd map, except the primary.
#
# @param poolname an existing pool
# @param objectname an objectname (may or may not exist)
# @param STDOUT the OSD id
# @return 0 on success, 1 on error
#
function get_not_primary() {
    local poolname=$1
    local objectname=$2

    local primary=$(get_primary $poolname $objectname)
    ceph --format xml osd map $poolname $objectname 2>/dev/null | \
        $XMLSTARLET sel -t -m "//acting/osd[not(.='$primary')]" -v . -n | \
        head -1
}

function test_get_not_primary() {
    local dir=$1

    setup $dir || return 1
    run_mon $dir a --osd_pool_default_size=2 || return 1
    run_osd $dir 0 || return 1
    run_osd $dir 1 || return 1
    wait_for_clean || return 1
    local primary=$(get_primary rbd GROUP)
    local not_primary=$(get_not_primary rbd GROUP)
    test $not_primary != $primary || return 1
    test $not_primary = 0 -o $not_primary = 1 || return 1
    teardown $dir || return 1
}

#######################################################################

##
# Run ceph-objectstore-tool against the OSD **id** using the data path
# **dir**. The OSD is killed with TERM prior to running
# ceph-objectstore-tool because access to the data path is
# exclusive. The OSD is restarted after the command completes. The
# objectstore_tool returns after all PG are active+clean again.
#
# @param dir the data path of the OSD
# @param id the OSD id
# @param ... arguments to ceph-objectstore-tool
# @param STDIN the input of ceph-objectstore-tool
# @param STDOUT the output of ceph-objectstore-tool
# @return 0 on success, 1 on error
#
function objectstore_tool() {
    local dir=$1
    shift
    local id=$1
    shift
    local osd_data=$dir/$id

    kill_daemons $dir TERM osd.$id >&2 < /dev/null || return 1
    ceph-objectstore-tool \
        --data-path $osd_data \
        --journal-path $osd_data/journal \
        "$@" || return 1
    activate_osd $dir $id >&2 || return 1
    wait_for_clean >&2
}

function test_objectstore_tool() {
    local dir=$1

    setup $dir || return 1
    run_mon $dir a --osd_pool_default_size=1 || return 1
    local osd=0
    run_osd $dir $osd || return 1
    wait_for_clean || return 1
    rados --pool rbd put GROUP /etc/group || return 1
    objectstore_tool $dir $osd GROUP get-bytes | \
        diff - /etc/group
    ! objectstore_tool $dir $osd NOTEXISTS get-bytes || return 1
    teardown $dir || return 1
}

#######################################################################

##
# Predicate checking if there is an ongoing recovery in the
# cluster. If any of the recovering_{keys,bytes,objects}_per_sec
# counters are reported by ceph status, it means recovery is in
# progress.
#
# @return 0 if recovery in progress, 1 otherwise
#
function get_is_making_recovery_progress() {
    local progress=$(ceph --format xml status 2>/dev/null | \
        $XMLSTARLET sel \
        -t -m "//pgmap/recovering_keys_per_sec" -v . -o ' ' \
        -t -m "//pgmap/recovering_bytes_per_sec" -v . -o ' ' \
        -t -m "//pgmap/recovering_objects_per_sec" -v .)
    test -n "$progress"
}

function test_get_is_making_recovery_progress() {
    local dir=$1

    setup $dir || return 1
    run_mon $dir a || return 1
    ! get_is_making_recovery_progress || return 1
    teardown $dir || return 1
}

#######################################################################

##
# Return the number of active PGs in the cluster. A PG is active if
# ceph pg dump pgs reports it both **active** and **clean** and that
# not **stale**.
#
# @param STDOUT the number of active PGs
# @return 0 on success, 1 on error
#
function get_num_active_clean() {
    local expression="("
    expression+="contains(.,'active') and "
    expression+="contains(.,'clean') and "
    expression+="not(contains(.,'stale'))"
    expression+=")"
    # xmlstarlet 1.3.0 (which is on Ubuntu precise)
    # add extra new lines that must be ignored with
    # grep -v '^$' 
    ceph --format xml pg dump pgs 2>/dev/null | \
        $XMLSTARLET sel -t -m "//pg_stat/state[$expression]" -v . -n | \
        grep -v '^$' | wc -l
}

function test_get_num_active_clean() {
    local dir=$1

    setup $dir || return 1
    run_mon $dir a --osd_pool_default_size=1 || return 1
    run_osd $dir 0 || return 1
    wait_for_clean || return 1
    local num_active_clean=$(get_num_active_clean)
    test "$num_active_clean" = $PG_NUM || return 1
    teardown $dir || return 1
}

#######################################################################

##
# Return the number of PGs in the cluster, according to
# ceph pg dump pgs.
#
# @param STDOUT the number of PGs
# @return 0 on success, 1 on error
#
function get_num_pgs() {
    ceph --format xml status 2>/dev/null | \
        $XMLSTARLET sel -t -m "//pgmap/num_pgs" -v .
}

function test_get_num_pgs() {
    local dir=$1

    setup $dir || return 1
    run_mon $dir a --osd_pool_default_size=1 || return 1
    run_osd $dir 0 || return 1
    wait_for_clean || return 1
    local num_pgs=$(get_num_pgs)
    test "$num_pgs" -gt 0 || return 1
    teardown $dir || return 1
}

#######################################################################

##
# Return the date and time of the last completed scrub for **pgid**,
# as reported by ceph pg dump pgs. Note that a repair also sets this
# date.
#
# @param pgid the id of the PG
# @param STDOUT the date and time of the last scrub
# @return 0 on success, 1 on error
#
function get_last_scrub_stamp() {
    local pgid=$1
    ceph --format xml pg dump pgs 2>/dev/null | \
        $XMLSTARLET sel -t -m "//pg_stat[pgid='$pgid']/last_scrub_stamp" -v .
}

function test_get_last_scrub_stamp() {
    local dir=$1

    setup $dir || return 1
    run_mon $dir a --osd_pool_default_size=1 || return 1
    run_osd $dir 0 || return 1
    wait_for_clean || return 1
    stamp=$(get_last_scrub_stamp 1.0)
    test -n "$stamp" || return 1
    teardown $dir || return 1
}

#######################################################################

##
# Predicate checking if the cluster is clean, i.e. all of its PGs are
# in a clean state (see get_num_active_clean for a definition).
#
# @return 0 if the cluster is clean, 1 otherwise
#
function is_clean() {
    num_pgs=$(get_num_pgs)
    test $num_pgs != 0 || return 1
    test $(get_num_active_clean) = $num_pgs || return 1
}

function test_is_clean() {
    local dir=$1

    setup $dir || return 1
    run_mon $dir a --osd_pool_default_size=1 || return 1
    run_osd $dir 0 || return 1
    ! is_clean || return 1
    wait_for_clean || return 1
    is_clean || return 1
    teardown $dir || return 1
}

#######################################################################

##
# Wait until the cluster becomes clean or if it does not make progress
# for $TIMEOUT seconds. The function **is_clean** is used to determine
# if the cluster is clean. Progress is measured either vian the
# **get_is_making_recovery_progress** predicate or if the number of
# clean PGs changes.
#
# @return 0 if the cluster is clean, 1 otherwise
#
function wait_for_clean() {
    local status=1
    local num_active_clean=$(get_num_active_clean)
    local cur_active_clean
    local -i timer=0
    while ! is_clean ; do
        if get_is_making_recovery_progress ; then
            timer=0
        elif (( timer >= $TIMEOUT )) ; then
            return 1
        fi

        cur_active_clean=$(get_num_active_clean)
        if test $cur_active_clean != $num_active_clean ; then
            timer=0
            num_active_clean=$cur_active_clean
        fi
        sleep 1
        (( timer++ ))
    done
    return 0
}

function test_wait_for_clean() {
    local dir=$1

    setup $dir || return 1
    run_mon $dir a --osd_pool_default_size=1 || return 1
    ! TIMEOUT=1 wait_for_clean || return 1
    run_osd $dir 0 || return 1
    wait_for_clean || return 1
    teardown $dir || return 1
}

#######################################################################

##
# Run repair on **pgid** and wait until it completes. The repair
# function will fail if repair does not complete within $TIMEOUT
# seconds. The repair is complete whenever the
# **get_last_scrub_stamp** function reports a timestamp different from
# the one stored before starting the repair.
#
# @param pgid the id of the PG
# @return 0 on success, 1 on error
#
function repair() {
    local pgid=$1
    local last_scrub=$(get_last_scrub_stamp $pgid)

    ceph pg repair $pgid
    for ((i=0; i < $TIMEOUT; i++)); do
        if test "$last_scrub" != "$(get_last_scrub_stamp $pgid)" ; then
            return 0
        fi
        sleep 1
    done
    return 1
}

function test_repair() {
    local dir=$1

    setup $dir || return 1
    run_mon $dir a --osd_pool_default_size=1 || return 1
    run_osd $dir 0 || return 1
    wait_for_clean || return 1
    repair 1.0 || return 1
    kill_daemons $dir KILL osd || return 1
    ! TIMEOUT=1 repair 1.0 || return 1
    teardown $dir || return 1
}

#######################################################################

##
# Call the **run** function (which must be defined by the caller) with
# the **dir** argument followed by the caller argument list. The
# **setup** function is called before the **run** function and the
# **teardown** function is called after to cleanup leftovers. The
# environment is prepared to protect the **run** function from
# pre-existing variables.
#
# The shell is required to display the function a line number whenever
# a statement is executed to facilitate debugging.
#
# @param dir directory in which all data is stored
# @param ... arguments passed transparently to **run**
# @return 0 on success, 1 on error
#
function main() {
    local dir=testdir/$1
    shift

    set -x
    PS4='${FUNCNAME[0]}: $LINENO: '
    #CEPH_HELPER_VERBOSE=true

    export PATH=:$PATH # make sure program from sources are prefered

    export CEPH_CONF=/dev/null
    unset CEPH_ARGS

    setup $dir || return 1
    local code
    if run $dir "$@" ; then
        code=0
    else
        code=1
    fi
    teardown $dir || return 1
    return $code
}

#######################################################################

function run_tests() {
    set -x
    PS4='${FUNCNAME[0]}: $LINENO: '

    export PATH=":$PATH"
    export CEPH_MON="127.0.0.1:7109"
    export CEPH_ARGS
    CEPH_ARGS+="--fsid=$(uuidgen) --auth-supported=none "
    CEPH_ARGS+="--mon-host=$CEPH_MON "
    export CEPH_CONF=/dev/null

    local funcs=${@:-$(set | sed -n -e 's/^\(test_[0-9a-z_]*\) .*/\1/p')}
    local dir=testdir/ceph-helpers

    for func in $funcs ; do
        $func $dir || return 1
    done
}

if test "$1" = TESTS ; then
    shift
    run_tests "$@"
fi

# Local Variables:
# compile-command: "cd .. ; make -j4 && test/ceph-helpers.sh TESTS # test_get_config"
# End:
