setopt SH_WORD_SPLIT

IFS=x

func1() {
  echo /$*/
  for i in $*; do echo -$i-; done
}
func1 "" ""

echo

func2() {
  echo /"$*"/
  for i in =$*=; do echo -$i-; done
}
func2 "" ""
