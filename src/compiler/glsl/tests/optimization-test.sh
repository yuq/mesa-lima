#!/bin/sh

if [ ! -z "$srcdir" ]; then
   compare_ir=`pwd`/tests/compare_ir.py
else
   compare_ir=./compare_ir.py
fi

if [ -z "$PYTHON2" ]; then
    PYTHON2=python2
fi

which $PYTHON2 >/dev/null
if [ $? -ne 0 ]; then
    echo "Could not find python2. Make sure that PYTHON2 variable is correctly set."
    exit 1
fi

if [ -z "$srcdir" -o -z "$abs_builddir" ]; then
    echo ""
    echo "Warning: you're invoking the script manually and things may fail."
    echo "Attempting to determine/set srcdir and abs_builddir variables."
    echo ""

    # Variable should point to the Makefile.glsl.am
    srcdir=./../../
    cd `dirname "$0"`
    # Variable should point to the folder two levels above glsl_test
    abs_builddir=`pwd`/../../
fi

total=0
pass=0

echo "======       Generating tests      ======"
for dir in tests/*/; do
    if [ -e "${dir}create_test_cases.py" ]; then
        cd $dir; $PYTHON2 create_test_cases.py; cd ..
    fi
    echo "$dir"
done

echo "====== Testing optimization passes ======"
for test in `find . -iname '*.opt_test'`; do
    echo -n "Testing $test..."
    (cd `dirname "$test"`; ./`basename "$test"`) > "$test.out" 2>&1
    total=$((total+1))
    if $PYTHON2 $PYTHON_FLAGS $compare_ir "$test.expected" "$test.out" >/dev/null 2>&1; then
        echo "PASS"
        pass=$((pass+1))
    else
        echo "FAIL"
        $PYTHON2 $PYTHON_FLAGS $compare_ir "$test.expected" "$test.out"
    fi
done

echo ""
echo "$pass/$total tests returned correct results"
echo ""

if [ $pass = $total ]; then
    exit 0
else
    exit 1
fi
