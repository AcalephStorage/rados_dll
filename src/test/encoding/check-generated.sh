#!/bin/sh -e

dir=$1

set -e

tmp1=`mktemp /tmp/typ-XXXXXXXXX`
tmp2=`mktemp /tmp/typ-XXXXXXXXX`
tmp3=`mktemp /tmp/typ-XXXXXXXXX`
tmp4=`mktemp /tmp/typ-XXXXXXXXX`

failed=0
numtests=0
echo "checking ceph-dencoder generated test instances..."
echo "numgen type"
for type in `./ceph-dencoder list_types`; do
    num=`./ceph-dencoder type $type count_tests`
    echo "$num $type"
    for n in `seq 1 1 $num 2>/dev/null`; do
	if ! ./ceph-dencoder type $type select_test $n encode decode; then
	    echo "**** $type test $n encode+decode check failed ****"
	    echo "   ceph-dencoder type $type select_test $n encode decode"
	    failed=$(($failed + 3))
	    continue
	fi

	./ceph-dencoder type $type select_test $n dump_json > $tmp1
	./ceph-dencoder type $type select_test $n encode decode dump_json > $tmp2
	./ceph-dencoder type $type select_test $n copy dump_json > $tmp3
	./ceph-dencoder type $type select_test $n copy_ctor dump_json > $tmp4

	if ! cmp $tmp1 $tmp2; then
	    echo "**** $type test $n dump_json check failed ****"
	    echo "   ceph-dencoder type $type select_test $n dump_json > $tmp1"
	    echo "   ceph-dencoder type $type select_test $n encode decode dump_json > $tmp2"
	    echo "   diff $tmp1 $tmp2"
	    failed=$(($failed + 1))
	fi

	if ! cmp $tmp1 $tmp3; then
	    echo "**** $type test $n copy dump_json check failed ****"
	    echo "   ceph-dencoder type $type select_test $n dump_json > $tmp1"
	    echo "   ceph-dencoder type $type select_test $n copy dump_json > $tmp2"
	    echo "   diff $tmp1 $tmp2"
	    failed=$(($failed + 1))
	fi

	if ! cmp $tmp1 $tmp4; then
	    echo "**** $type test $n copy_ctor dump_json check failed ****"
	    echo "   ceph-dencoder type $type select_test $n dump_json > $tmp1"
	    echo "   ceph-dencoder type $type select_test $n copy_ctor dump_json > $tmp2"
	    echo "   diff $tmp1 $tmp2"
	    failed=$(($failed + 1))
	fi

	./ceph-dencoder type $type select_test $n encode export $tmp1
	./ceph-dencoder type $type select_test $n encode decode encode export $tmp2
	if ! cmp $tmp1 $tmp2; then
	    echo "**** $type test $n binary reencode check failed ****"
	    echo "   ceph-dencoder type $type select_test $n encode export $tmp1"
	    echo "   ceph-dencoder type $type select_test $n encode decode encode export $tmp2"
	    echo "   cmp $tmp1 $tmp2"
	    failed=$(($failed + 1))
	fi


	numtests=$(($numtests + 3))
    done
done

rm -f $tmp1 $tmp2 $tmp3 $tmp4

if [ $failed -gt 0 ]; then
    echo "FAILED $failed / $numtests tests."
    exit 1
fi
echo "passed $numtests tests."
