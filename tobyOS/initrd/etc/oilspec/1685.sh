# - mksh doesn't support [[:punct:]] ?
# - python shell fails because \[ not supported!
touch _tmp/\[abc\] _tmp/\?
echo _tmp/\[???\] _tmp/\?
