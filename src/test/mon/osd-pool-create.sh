#!/bin/bash
#
# Copyright (C) 2013,2014 Cloudwatt <libre.licensing@cloudwatt.com>
# Copyright (C) 2014 Red Hat <contact@redhat.com>
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
source test/mon/mon-test-helpers.sh

function expect_false()
{
    set -x
    if "$@"; then return 1; else return 0; fi
}

function run() {
    local dir=$1

    export CEPH_MON="127.0.0.1:7105"
    export CEPH_ARGS
    CEPH_ARGS+="--fsid=$(uuidgen) --auth-supported=none "
    CEPH_ARGS+="--mon-host=$CEPH_MON "

    FUNCTIONS=${FUNCTIONS:-$(set | sed -n -e 's/^\(TEST_[0-9a-z_]*\) .*/\1/p')}
    for TEST_function in $FUNCTIONS ; do
        setup $dir || return 1
        $TEST_function $dir || return 1
        teardown $dir || return 1
    done
}

function TEST_default_deprectated_0() {
    local dir=$1
    # explicitly set the default crush rule
    expected=66
    run_mon $dir a --public-addr $CEPH_MON \
        --osd_pool_default_crush_replicated_ruleset $expected
    ./ceph --format json osd dump | grep '"crush_ruleset":'$expected
    CEPH_ARGS='' ./ceph --admin-daemon $dir/a/ceph-mon.a.asok log flush || return 1
    ! grep "osd_pool_default_crush_rule is deprecated " $dir/a/log || return 1
}

function TEST_default_deprectated_1() {
    local dir=$1
    # explicitly set the default crush rule using deprecated option
    expected=55
    run_mon $dir a --public-addr $CEPH_MON \
        --osd_pool_default_crush_rule $expected
    ./ceph --format json osd dump | grep '"crush_ruleset":'$expected
    CEPH_ARGS='' ./ceph --admin-daemon $dir/a/ceph-mon.a.asok log flush || return 1
    grep "osd_pool_default_crush_rule is deprecated " $dir/a/log || return 1
}

function TEST_default_deprectated_2() {
    local dir=$1
    expected=77
    unexpected=33
    run_mon $dir a --public-addr $CEPH_MON \
        --osd_pool_default_crush_rule $expected \
        --osd_pool_default_crush_replicated_ruleset $unexpected
    ./ceph --format json osd dump | grep '"crush_ruleset":'$expected
    ! ./ceph --format json osd dump | grep '"crush_ruleset":'$unexpected || return 1
    CEPH_ARGS='' ./ceph --admin-daemon $dir/a/ceph-mon.a.asok log flush || return 1
    grep "osd_pool_default_crush_rule is deprecated " $dir/a/log || return 1
}

# Before http://tracker.ceph.com/issues/8307 the invalid profile was created
function TEST_erasure_invalid_profile() {
    local dir=$1
    run_mon $dir a --public-addr $CEPH_MON
    local poolname=pool_erasure
    local notaprofile=not-a-valid-erasure-code-profile
    ! ./ceph osd pool create $poolname 12 12 erasure $notaprofile || return 1
    ! ./ceph osd erasure-code-profile ls | grep $notaprofile || return 1
}

function TEST_erasure_crush_rule() {
    local dir=$1
    run_mon $dir a --public-addr $CEPH_MON
    # 
    # choose the crush ruleset used with an erasure coded pool
    #
    local crush_ruleset=myruleset
    ! ./ceph osd crush rule ls | grep $crush_ruleset || return 1
    ./ceph osd crush rule create-erasure $crush_ruleset
    ./ceph osd crush rule ls | grep $crush_ruleset
    local poolname
    poolname=pool_erasure1
    ! ./ceph --format json osd dump | grep '"crush_ruleset":1' || return 1
    ./ceph osd pool create $poolname 12 12 erasure default $crush_ruleset
    ./ceph --format json osd dump | grep '"crush_ruleset":1' || return 1
    #
    # a crush ruleset by the name of the pool is implicitly created
    #
    poolname=pool_erasure2
    ./ceph osd erasure-code-profile set myprofile
    ./ceph osd pool create $poolname 12 12 erasure myprofile
    ./ceph osd crush rule ls | grep $poolname || return 1
    #
    # a non existent crush ruleset given in argument is an error
    # http://tracker.ceph.com/issues/9304
    #
    poolname=pool_erasure3
    ! ./ceph osd pool create $poolname 12 12 erasure myprofile INVALIDRULESET || return 1
}

function TEST_erasure_code_profile_default() {
    local dir=$1
    run_mon $dir a --public-addr $CEPH_MON
    ./ceph osd erasure-code-profile rm default || return 1
    ! ./ceph osd erasure-code-profile ls | grep default || return 1
    ./ceph osd pool create $poolname 12 12 erasure default
    ./ceph osd erasure-code-profile ls | grep default || return 1
}

function TEST_erasure_crush_stripe_width() {
    local dir=$1
    # the default stripe width is used to initialize the pool
    run_mon $dir a --public-addr $CEPH_MON
    stripe_width=$(./ceph-conf --show-config-value osd_pool_erasure_code_stripe_width)
    ./ceph osd pool create pool_erasure 12 12 erasure
    ./ceph --format json osd dump | tee $dir/osd.json
    grep '"stripe_width":'$stripe_width $dir/osd.json > /dev/null || return 1
}

function TEST_erasure_crush_stripe_width_padded() {
    local dir=$1
    # setting osd_pool_erasure_code_stripe_width modifies the stripe_width
    # and it is padded as required by the default plugin
    profile+=" plugin=jerasure"
    profile+=" technique=reed_sol_van"
    k=4
    profile+=" k=$k"
    profile+=" m=2"
    expected_chunk_size=2048
    actual_stripe_width=$(($expected_chunk_size * $k))
    desired_stripe_width=$(($actual_stripe_width - 1))
    run_mon $dir a --public-addr $CEPH_MON \
        --osd_pool_erasure_code_stripe_width $desired_stripe_width \
        --osd_pool_default_erasure_code_profile "$profile"
    ./ceph osd pool create pool_erasure 12 12 erasure
    ./ceph osd dump | tee $dir/osd.json
    grep "stripe_width $actual_stripe_width" $dir/osd.json > /dev/null || return 1
}

function TEST_erasure_code_pool() {
    local dir=$1
    run_mon $dir a --public-addr $CEPH_MON
    ./ceph --format json osd dump > $dir/osd.json
    local expected='"erasure_code_profile":"default"'
    ! grep "$expected" $dir/osd.json || return 1
    ./ceph osd pool create erasurecodes 12 12 erasure
    ./ceph --format json osd dump | tee $dir/osd.json
    grep "$expected" $dir/osd.json > /dev/null || return 1

    ./ceph osd pool create erasurecodes 12 12 erasure 2>&1 | \
        grep 'already exists' || return 1
    ./ceph osd pool create erasurecodes 12 12 2>&1 | \
        grep 'cannot change to type replicated' || return 1
}

function TEST_replicated_pool_with_ruleset() {
    local dir=$1
    run_mon $dir a --public-addr $CEPH_MON
    local ruleset=ruleset0
    local root=host1
    ./ceph osd crush add-bucket $root host
    local failure_domain=osd
    local poolname=mypool
    ./ceph osd crush rule create-simple $ruleset $root $failure_domain || return 1
    ./ceph osd crush rule ls | grep $ruleset
    ./ceph osd pool create $poolname 12 12 replicated $ruleset 2>&1 | \
        grep "pool 'mypool' created" || return 1
    rule_id=`./ceph osd crush rule dump $ruleset | grep "rule_id" | awk -F[' ':,] '{print $4}'`
    ./ceph osd pool get $poolname crush_ruleset  2>&1 | \
        grep "crush_ruleset: $rule_id" || return 1
    #non-existent crush ruleset
    ./ceph osd pool create newpool 12 12 replicated non-existent 2>&1 | \
        grep "doesn't exist" || return 1
}

function TEST_erasure_code_pool_lrc() {
    local dir=$1
    run_mon $dir a --public-addr $CEPH_MON

    ./ceph osd erasure-code-profile set LRCprofile \
             plugin=lrc \
             mapping=DD_ \
             layers='[ [ "DDc", "" ] ]' || return 1

    ./ceph --format json osd dump > $dir/osd.json
    local expected='"erasure_code_profile":"LRCprofile"'
    local poolname=erasurecodes
    ! grep "$expected" $dir/osd.json || return 1
    ./ceph osd pool create $poolname 12 12 erasure LRCprofile
    ./ceph --format json osd dump | tee $dir/osd.json
    grep "$expected" $dir/osd.json > /dev/null || return 1
    ./ceph osd crush rule ls | grep $poolname || return 1
}

function TEST_replicated_pool() {
    local dir=$1
    run_mon $dir a --public-addr $CEPH_MON
    ./ceph osd pool create replicated 12 12 replicated replicated_ruleset 2>&1 | \
        grep "pool 'replicated' created" || return 1
    ./ceph osd pool create replicated 12 12 replicated replicated_ruleset 2>&1 | \
        grep 'already exists' || return 1
    ! ./ceph osd pool create replicated0 12 12 replicated INVALIDRULESET
    # default is replicated
    ./ceph osd pool create replicated1 12 12 2>&1 | \
        grep "pool 'replicated1' created" || return 1
    # default is replicated, pgp_num = pg_num
    ./ceph osd pool create replicated2 12 2>&1 | \
        grep "pool 'replicated2' created" || return 1
    ./ceph osd pool create replicated 12 12 erasure 2>&1 | \
        grep 'cannot change to type erasure' || return 1
}

function TEST_no_pool_delete() {
    local dir=$1
    run_mon $dir a --public-addr $CEPH_MON
    ./ceph osd pool create foo 1
    ./ceph tell mon.a injectargs -- --no-mon-allow-pool-delete
    expect_false ./ceph osd pool delete foo foo --yes-i-really-really-mean-it
    ./ceph tell mon.a injectargs -- --mon-allow-pool-delete
    ./ceph osd pool delete foo foo --yes-i-really-really-mean-it
}


main osd-pool-create

# Local Variables:
# compile-command: "cd ../.. ; make -j4 && test/mon/osd-pool-create.sh"
# End:
