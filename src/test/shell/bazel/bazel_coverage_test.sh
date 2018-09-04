#!/bin/bash
#
# Copyright 2015 The Bazel Authors. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set -eu

# Load the test setup defined in the parent directory
CURRENT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${CURRENT_DIR}/../integration_test_setup.sh" \
  || { echo "integration_test_setup.sh not found!" >&2; exit 1; }

function setup_cc_source_files() {
  cat << EOF > BUILD
cc_library(
    name = "a",
    srcs = ["a.cc"],
    hdrs = ["a.h"],
)

cc_test(
    name = "t",
    srcs = ["t.cc"],
    deps = [":a"],
)
EOF

  cat << EOF > a.h
int a(bool what);
EOF

  cat << EOF > a.cc
#include "a.h"

int a(bool what) {
  if (what) {
    return 1;
  } else {
    return 2;
  }
}
EOF

  cat << EOF > t.cc
#include <stdio.h>
#include "a.h"

int main(void) {
  a(true);
}
EOF
}

# Returns the expected coverage result for a given source file and format.
#
# - source_file The name of the source file for which the expected coverage
#               should be returned. The accepted values are a.cc and t.cc.
#               For other values the method fails.
function get_expected_coverage() {
    local source_file="${1}"

    # The expected results can be constructed manually by following the lcov
    # documentation and manually checking what lines of code are covered when
    # running the test.
    # For details about the lcov format see
    # http://ltp.sourceforge.net/coverage/lcov/geninfo.1.php
    # The newlines are replaced with commas to facilitate comparing the
    # expected values with the actual results.

    # The expected coverage result for a.cc in lcov format.
    local expected_result_a_cc="TN:
SF:coverage_srcs/a.cc
FN:3,_Z1ab
FNDA:1,_Z1ab
FNF:1
FNH:1
DA:3,1
DA:4,1
DA:5,1
DA:7,0
LF:4
LH:3
end_of_record"

    # The expected coverage result for t.cc in lcov format.
    local expected_result_t_cc="TN:
SF:coverage_srcs/t.cc
FN:4,main
FNDA:1,main
FNF:1
FNH:1
DA:4,1
DA:5,1
DA:6,1
LF:3
LH:3
end_of_record"

    # Return the expected result for the given source file.
    case "$source_file" in
        ("a.cc") echo "$expected_lcov_result_a_cc" ;;
        ("t.cc") echo "$expected_lcov_result_t_cc" ;;
        (*) fail "Coverage cannot be requested for file $source_file" ;;
    esac
}

# Asserts if the given expected coverage result is included in the given output
# file.
#
# - expected_coverage The expected result that must be included in the output.
# - output_file       The location of the coverage output file.
function assert_coverage_result() {
    local expected_coverage="${1}"
    local output_file="${2}"

    # Replace newlines with commas to facilitate the assertion.
    local expected_coverage_no_newlines=$( echo "$expected_coverage" | tr '\n' ',' )
    local output_file_no_newlines=$( cat "$output_file" | tr '\n' ',' )

    ( echo $output_file_no_newlines | grep  $expected_coverage_no_newlines ) \
        || fail "Expected coverage result
<$expected_coverage>
was not found in actual coverage report:
<$( cat $output_file )>"
}

function get_coverage_file() {
  local ending_part=$(sed -n -e '/PASSED/,$p' $TEST_log)

  local coverage_file_path=$(grep -Eo "/[/a-zA-Z0-9\.\_\-]+\.dat$" <<< "$ending_part")
  [ -e $coverage_file_path ] || fail "Coverage output file does not exist!"
  echo "$coverage_file_path"
}

function test_cc_test_coverage_lcov() {
  local -r lcov_location=$(which lcov)
  if [[ ! -x ${lcov_location:-/usr/bin/lcov} ]]; then
    echo "lcov not installed. Skipping test."
    return
  fi

  setup_cc_source_files

  bazel coverage --test_output=all --build_event_text_file=bep.txt //:t \
      &>$TEST_log || fail "Coverage for //:t failed"

  local coverage_output_file=$( get_coverage_file )

  # Check the expected coverage for a.cc in the coverage file.
  assert_coverage_result "$(get_expected_coverage a.cc)" "$coverage_output_file"
  # Check the expected coverage for a.cc in the coverage file.
  assert_coverage_result "$(get_expected_coverage t.cc)" "$coverage_output_file"

  # Verify that this is also true for cached coverage actions.
  bazel coverage --test_output=all --build_event_text_file=bep.txt //:t \
      &>$TEST_log || fail "Coverage for //:t failed"
  expect_log '//:t.*cached'
  # Verify the files are reported correctly in the build event protocol.
  assert_contains 'name: "test.lcov"' bep.txt
  assert_contains 'name: "baseline.lcov"' bep.txt
}

function test_cc_test_coverage_gcov() {
  local -r lcov_location=$(which lcov)
  if [[ ! -x ${lcov_location:-/usr/bin/lcov} ]]; then
    echo "lcov not installed. Skipping test."
    return
  fi

  setup_cc_source_files

  bazel coverage --experimental_use_gcov_coverage --test_output=all \
     --build_event_text_file=bep.txt //:t &>$TEST_log || fail "Coverage for //:t failed"

  local coverage_file_path=$( get_coverage_file )
  # Check the expected coverage for a.cc in the coverage file.
  assert_coverage_result "$(get_expected_coverage a.cc)" "$coverage_file_path"
  # Check the expected coverage for a.cc in the coverage file.
  assert_coverage_result "$(get_expected_coverage t.cc)" "$coverage_file_path"

  # Verify that this is also true for cached coverage actions.
  bazel coverage --experimental_use_gcov_coverage --test_output=all --build_event_text_file=bep.txt //:t \
      &>$TEST_log || fail "Coverage for //:t failed"
  expect_log '//:t.*cached'
  # Verify the files are reported correctly in the build event protocol.
  assert_contains 'name: "test.lcov"' bep.txt
  assert_contains 'name: "baseline.lcov"' bep.txt
}

function test_failed_coverage() {
  local -r LCOV=$(which lcov)
  if [[ ! -x ${LCOV:-/usr/bin/lcov} ]]; then
    echo "lcov not installed. Skipping test."
    return
  fi

  cat << EOF > BUILD
cc_library(
    name = "a",
    srcs = ["a.cc"],
    hdrs = ["a.h"],
)

cc_test(
    name = "t",
    srcs = ["t.cc"],
    deps = [":a"],
)
EOF

  cat << EOF > a.h
int a();
EOF

  cat << EOF > a.cc
#include "a.h"

int a() {
  return 1;
}
EOF

  cat << EOF > t.cc
#include <stdio.h>
#include "a.h"

int main(void) {
  return a();
}
EOF

  bazel coverage --test_output=all --build_event_text_file=bep.txt //:t \
      &>$TEST_log && fail "Expected test failure"

  # Verify that coverage data is still reported.
  assert_contains 'name: "test.lcov"' bep.txt
}

function test_java_test_coverage() {

  cat <<EOF > BUILD
java_test(
    name = "test",
    srcs = glob(["src/test/**/*.java"]),
    test_class = "com.example.TestCollatz",
    deps = [":collatz-lib"],
)

java_library(
    name = "collatz-lib",
    srcs = glob(["src/main/**/*.java"]),
)
EOF

  mkdir -p src/main/com/example
  cat <<EOF > src/main/com/example/Collatz.java
package com.example;

public class Collatz {

  public static int getCollatzFinal(int n) {
    if (n == 1) {
      return 1;
    }
    if (n % 2 == 0) {
      return getCollatzFinal(n / 2);
    } else {
      return getCollatzFinal(n * 3 + 1);
    }
  }

}
EOF

  mkdir -p src/test/com/example
  cat <<EOF > src/test/com/example/TestCollatz.java
package com.example;

import static org.junit.Assert.assertEquals;
import org.junit.Test;

public class TestCollatz {

  @Test
  public void testGetCollatzFinal() {
    assertEquals(Collatz.getCollatzFinal(1), 1);
    assertEquals(Collatz.getCollatzFinal(5), 1);
    assertEquals(Collatz.getCollatzFinal(10), 1);
    assertEquals(Collatz.getCollatzFinal(21), 1);
  }

}
EOF

  bazel coverage --test_output=all //:test &>$TEST_log || fail "Coverage for //:test failed"
  cat $TEST_log
  local coverage_file_path=$( get_coverage_file )

  cat <<EOF > result.dat
SF:com/example/Collatz.java
FN:3,com/example/Collatz::<init> ()V
FN:6,com/example/Collatz::getCollatzFinal (I)I
FNDA:0,com/example/Collatz::<init> ()V
FNDA:1,com/example/Collatz::getCollatzFinal (I)I
FNF:2
FNH:1
BA:6,2
BA:9,2
BRF:2
BRH:2
DA:3,0
DA:6,3
DA:7,2
DA:9,4
DA:10,5
DA:12,7
LH:5
LF:6
end_of_record
EOF

  diff result.dat "$coverage_file_path" >> $TEST_log
  if ! cmp result.dat $coverage_file_path; then
    fail "Coverage output file is different with expected"
  fi
}

function test_java_test_coverage_combined_report() {

  cat <<EOF > BUILD
java_test(
    name = "test",
    srcs = glob(["src/test/**/*.java"]),
    test_class = "com.example.TestCollatz",
    deps = [":collatz-lib"],
)

java_library(
    name = "collatz-lib",
    srcs = glob(["src/main/**/*.java"]),
)
EOF

  mkdir -p src/main/com/example
  cat <<EOF > src/main/com/example/Collatz.java
package com.example;

public class Collatz {

  public static int getCollatzFinal(int n) {
    if (n == 1) {
      return 1;
    }
    if (n % 2 == 0) {
      return getCollatzFinal(n / 2);
    } else {
      return getCollatzFinal(n * 3 + 1);
    }
  }

}
EOF

  mkdir -p src/test/com/example
  cat <<EOF > src/test/com/example/TestCollatz.java
package com.example;

import static org.junit.Assert.assertEquals;
import org.junit.Test;

public class TestCollatz {

  @Test
  public void testGetCollatzFinal() {
    assertEquals(Collatz.getCollatzFinal(1), 1);
    assertEquals(Collatz.getCollatzFinal(5), 1);
    assertEquals(Collatz.getCollatzFinal(10), 1);
    assertEquals(Collatz.getCollatzFinal(21), 1);
  }

}
EOF

  bazel coverage --test_output=all //:test --coverage_report_generator=@bazel_tools//tools/test/LcovMerger/java/com/google/devtools/lcovmerger:Main --combined_report=lcov &>$TEST_log \
   || echo "Coverage for //:test failed"

  cat <<EOF > result.dat
SF:com/example/Collatz.java
FN:3,com/example/Collatz::<init> ()V
FN:6,com/example/Collatz::getCollatzFinal (I)I
FNDA:0,com/example/Collatz::<init> ()V
FNDA:1,com/example/Collatz::getCollatzFinal (I)I
FNF:2
FNH:1
BA:6,2
BA:9,2
BRF:2
BRH:2
DA:3,0
DA:6,3
DA:7,2
DA:9,4
DA:10,5
DA:12,7
LH:5
LF:6
end_of_record
EOF

  if ! cmp result.dat ./bazel-out/_coverage/_coverage_report.dat; then
    diff result.dat bazel-out/_coverage/_coverage_report.dat >> $TEST_log
    fail "Coverage output file is different with expected"
  fi
}

function test_java_test_java_import_coverage() {

  cat <<EOF > BUILD
java_test(
    name = "test",
    srcs = glob(["src/test/**/*.java"]),
    test_class = "com.example.TestCollatz",
    deps = [":collatz-import"],
)

java_import(
    name = "collatz-import",
    jars = [":libcollatz-lib.jar"],
)

java_library(
    name = "collatz-lib",
    srcs = glob(["src/main/**/*.java"]),
)
EOF

  mkdir -p src/main/com/example
  cat <<EOF > src/main/com/example/Collatz.java
package com.example;

public class Collatz {

  public static int getCollatzFinal(int n) {
    if (n == 1) {
      return 1;
    }
    if (n % 2 == 0) {
      return getCollatzFinal(n / 2);
    } else {
      return getCollatzFinal(n * 3 + 1);
    }
  }

}
EOF

  mkdir -p src/test/com/example
  cat <<EOF > src/test/com/example/TestCollatz.java
package com.example;

import static org.junit.Assert.assertEquals;
import org.junit.Test;

public class TestCollatz {

  @Test
  public void testGetCollatzFinal() {
    assertEquals(Collatz.getCollatzFinal(1), 1);
    assertEquals(Collatz.getCollatzFinal(5), 1);
    assertEquals(Collatz.getCollatzFinal(10), 1);
    assertEquals(Collatz.getCollatzFinal(21), 1);
  }

}
EOF

  bazel coverage --test_output=all --experimental_java_coverage //:test &>$TEST_log || fail "Coverage for //:test failed"
  local coverage_file_path=$( get_coverage_file )

  cat <<EOF > result.dat
SF:src/main/com/example/Collatz.java
FN:3,com/example/Collatz::<init> ()V
FN:6,com/example/Collatz::getCollatzFinal (I)I
FNDA:0,com/example/Collatz::<init> ()V
FNDA:1,com/example/Collatz::getCollatzFinal (I)I
FNF:2
FNH:1
BA:6,2
BA:9,2
BRF:2
BRH:2
DA:3,0
DA:6,3
DA:7,2
DA:9,4
DA:10,5
DA:12,7
LH:5
LF:6
end_of_record
EOF
  diff result.dat "$coverage_file_path" >> $TEST_log
  cmp result.dat "$coverage_file_path" || fail "Coverage output file is different than the expected file"
}

function test_sh_test_coverage() {
  cat <<EOF > BUILD
sh_test(
    name = "orange-sh",
    srcs = ["orange-test.sh"],
    data = ["//java/com/google/orange:orange-bin"]
)
EOF
  cat <<EOF > orange-test.sh
#!/bin/bash

java/com/google/orange/orange-bin
EOF
  chmod +x orange-test.sh

  mkdir -p java/com/google/orange

  cat <<EOF > java/com/google/orange/BUILD
package(default_visibility = ["//visibility:public"])

java_binary(
    name = "orange-bin",
    srcs = ["orangeBin.java"],
    main_class = "com.google.orange.orangeBin",
    deps = [":orange-lib"],
)

java_library(
    name = "orange-lib",
    srcs = ["orangeLib.java"],
)
EOF

  cat <<EOF > java/com/google/orange/orangeLib.java
package com.google.orange;

public class orangeLib {

  public void print() {
    System.out.println("orange prints a message!");
  }
}
EOF

  cat <<EOF > java/com/google/orange/orangeBin.java
package com.google.orange;

public class orangeBin {
  public static void main(String[] args) {
    orangeLib orange = new orangeLib();
    orange.print();
  }
}
EOF

  bazel coverage --test_output=all //:orange-sh &>$TEST_log || fail "Coverage for //:orange-sh failed"

  local coverage_file_path=$( get_coverage_file )

  cat <<EOF > result.dat
SF:com/google/orange/orangeBin.java
FN:3,com/google/orange/orangeBin::<init> ()V
FN:5,com/google/orange/orangeBin::main ([Ljava/lang/String;)V
FNDA:0,com/google/orange/orangeBin::<init> ()V
FNDA:1,com/google/orange/orangeBin::main ([Ljava/lang/String;)V
FNF:2
FNH:1
DA:3,0
DA:5,4
DA:6,2
DA:7,1
LH:3
LF:4
end_of_record
SF:com/google/orange/orangeLib.java
FN:3,com/google/orange/orangeLib::<init> ()V
FN:6,com/google/orange/orangeLib::print ()V
FNDA:1,com/google/orange/orangeLib::<init> ()V
FNDA:1,com/google/orange/orangeLib::print ()V
FNF:2
FNH:2
DA:3,3
DA:6,3
DA:7,1
LH:3
LF:3
end_of_record
EOF
  diff result.dat "$coverage_file_path" >> $TEST_log
  cmp result.dat "$coverage_file_path" || fail "Coverage output file is different than the expected file"
}

run_suite "test tests"
