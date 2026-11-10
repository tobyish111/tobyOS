case $SH in dash|mksh|zsh) exit ;; esac

git-branch-merged() {
  cat <<EOF
  foo
* bar
  baz
  master
EOF
}

shopt -s lastpipe  # required for bash, not OSH

branches=()  # dangerous when set -e is on
git-branch-merged | while read -r line; do
  line=${line# *}  # strip leading spaces
  if [[ $line != 'master' && ! ${line:0:1} == '*' ]]; then
    branches+=("$line")
  fi
done

if [[ ${#branches[@]} -eq 0 ]]; then
  echo "No merged branches"
else
  echo git branch -D "${branches[@]}"
fi
