case $SH in dash|bash*|mksh|zsh) exit ;; esac

shopt -s ysh:all

proc proc_that_evals(; ; ;b) {
  for i in 1 2; do
    echo $i
    call io->eval(b)
  done
  echo 'end func'
}

var cases = [
  ['break', ^(break)],
  ['continue', ^(continue)],
  ['return', ^(return)],
  ['false', ^(false)],
]

for test_case in (cases) {
  var code_str, block = test_case
  echo "--- $code_str"
  proc_that_evals (; ; block)
}
echo status=$?
