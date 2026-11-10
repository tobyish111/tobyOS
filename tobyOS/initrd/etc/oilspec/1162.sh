foo=bar true
echo foo=$foo

true() {
  echo true func
}
foo=bar true
echo foo=$foo



# POSIX rule about special builtins pointed at:
#
# https://www.reddit.com/r/oilshell/comments/5ykpi3/oildev_is_alive/
