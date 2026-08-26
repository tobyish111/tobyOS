# bash - ambiguous redirect -- yeah I want this error
#   - But I want it at PARSE time?  So is there a special DollarAtPart?
#     MultipleArgsPart?
# mksh - tries to create '_tmp/var-sub1 _tmp/var-sub2'
# dash - tries to create '_tmp/var-sub1 _tmp/var-sub2'
fun() {
  echo hi > "$@"
}
fun _tmp/var-sub1 _tmp/var-sub2
