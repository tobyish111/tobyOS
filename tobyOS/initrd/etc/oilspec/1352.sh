set -o nounset
f() {
  local foo
  echo "[$foo]"
}
f
# zsh doesn't support nounset?
