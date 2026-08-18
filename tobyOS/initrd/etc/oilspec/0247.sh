# Note: This and next tests have originally been in "spec/assign.test.sh" and
# compared the behavior of OSH's BashAssoc and Bash's indexed array.  After
# supporting "arr=([index]=value)" for indexed arrays, the test was adjusted
# and copied here. See also the corresponding tests in "spec/assign.test.sh"
a=([k1]=v1 [k2]=v2)
echo ${a["k1"]}
echo ${a["k2"]}
