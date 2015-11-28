#!/bin/bash

sds=$1
if [ -z $sds ] ; then
	sds=`which svndumpsanitizer 2>/dev/null`
	if [ -z $sds ] ; then
		echo "Usage: $0 path/to/svndumpsanitizer" >&2
		exit 1
	fi
fi

if [ ! -f $sds ] ; then
	echo "Usage: $0 path/to/svndumpsanitizer" >&2
	exit 1
fi

if [[ "${sds:0:1}" != "/" ]] ; then
	if [[ "${sds:0:2}" == "./" ]] ; then
		sds=$PWD/${sds:2}
	else
		sds=$PWD/$sds
	fi
fi

fail=0
pass=0
messages=""
pushd `dirname $0`
for subdir in `ls -l | grep ^d | sed 's/.*[ \t]//'` ; do
	pushd $subdir
	if [ -e temp.dump ] ; then
		rm -f temp.dump
	fi
	for f in source.dump facit.dump options.txt ; do
		if [ ! -f $f ] ; then
			messages=$messages"$subdir: File $f missing from test.\n"
			((fail++))
			popd
			continue 2
		fi
	done
	$sds -i source.dump -o test.dump `cat options.txt`
	if [ -f timestamp.txt ] ; then
		ts1=`diff facit.dump test.dump | head -2 | tail -1 | sed 's/.* //'`
		ts2=`cat timestamp.txt`
		if [ `diff facit.dump test.dump | wc -l` -ne 4 ] ; then
			messages=$messages"$subdir: Test target mismatch.\n"
			((fail++))
		elif [[ "$ts1" != "$ts2" ]] ; then
			messages=$messages"$subdir: Test target mismatch.\n"
			((fail++))
		elif [ `stat -c%s facit.dump` -ne `stat -c%s test.dump` ] ; then
			messages=$messages"$subdir: Test target mismatch.\n"
			((fail++))
		else
			((pass++))
		fi
	else
		md1=`md5sum facit.dump | sed 's/[ \t].*//'`
		md2=`md5sum test.dump | sed 's/[ \t].*//'`
		if [[ "$md1" != "$md2" ]] ; then
			messages=$messages"$subdir: Test target mismatch.\n"
			((fail++))
		else
			((pass++))
		fi
	fi
	popd
done
popd
echo -e "\n$pass tests passed out of $((pass + fail))"
if [ $fail -gt 0 ] ; then
	echo "Messages from failed tests below:"
	echo -e $messages
	exit 1
fi
exit 0
