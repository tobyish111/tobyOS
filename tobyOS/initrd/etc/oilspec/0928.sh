# When set, bash always appends when exiting, no matter what. 
# When unset, bash will append anyway as long the # of new commands < the hist length
# Either way, the file is truncated to HISTFILESIZE afterwards.
# osh always appends

cd $TMP 

export HISTSIZE=10
export HISTFILESIZE=1000
export HISTFILE=tmp

histappend_test() {
  local histopt
  if [[ "$1" == true ]]; then
    histopt='shopt -s histappend'
  else
    histopt='shopt -u histappend'
  fi

  printf "cmd orig%s\n" {1..10} > tmp

  $SH --norc -i <<EOF
  HISTSIZE=2 # Stifle the history down to 2 commands
  $histopt
  # Now run >2 commands to trigger bash's overwrite behavior
  echo cmd new1 > /dev/null
  echo cmd new2 > /dev/null
  echo cmd new3 > /dev/null
EOF

  case $SH in bash) echo '^D' ;; esac
}

# If we force histappend, bash won't overwrite the history file
histappend_test true
grep "orig" tmp > /dev/null
echo status=$?

# If we don't force histappend, bash will overwrite the history file when the number of cmds exceeds HISTSIZE
histappend_test false
grep "orig" tmp > /dev/null
echo status=$?
