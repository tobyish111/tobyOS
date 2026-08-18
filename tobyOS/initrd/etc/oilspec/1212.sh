trap 'echo err' ERR

passing() {
  false  # line 4
  true
}

failing() {
  true
  false
}

passing
failing

set -o errtrace

echo 'now with errtrace'
passing
failing

echo ok
