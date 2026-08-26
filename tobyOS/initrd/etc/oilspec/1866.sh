typeset -A bashup_ev_r
bashup_ev_r['foo']=bar

p() {
  local s=foo
  local -n e=bashup_ev["$s"] f=bashup_ev_r["$s"]
  # Different!
  #local e=bashup_ev["$s"] f=bashup_ev_r["$s"]
  argv.py "$f"
}
p
