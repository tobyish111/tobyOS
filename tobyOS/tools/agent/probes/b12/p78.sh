exec 2>&1
f1() {
  cat <<EOF
body $x
EOF
  cat <<-'Q' >/dev/null
	tabbed \
	line
	Q
  echo after
}
declare -f f1
f2() { g() { echo inner; }; g; }
declare -f f2
function do { echo reserved-name; }
declare -f do
f3() { if a; then b; elif c; then d; else e; fi; }
type f3
f4() { case $1 in (a) x;; b|c) ;; *) y;& z) w;;& esac; }
declare -f f4
f5() { echo $(echo a | tr a b) "$(ls; echo x)" `echo bt`; }
declare -f f5
f6() { x=( 1 2 ) && echo ${x[@]} || { echo no; } ; echo a & echo b; }
declare -f f6
f7() { for ((;;)); do break; done; select v in a b; do break; done; }
declare -f f7
f8() { echo $'a\tb' $"loc" "q'q" 'd"q'; }
declare -f f8
f9() { [[ ! -f x && ( a == b || -z "" ) ]]; (( a = 1, b++ )); }
declare -f f9
f10() { exec 3>&1 4<&- 5<>f 2>>log >|g &>/dev/null; cat <<<word; }
declare -f f10
