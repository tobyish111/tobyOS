((/usr/bin/cat </dev/zero; echo $? >&7) | true) 7>&1

((cat </dev/zero; echo $? >&7) | true) 7>&1
