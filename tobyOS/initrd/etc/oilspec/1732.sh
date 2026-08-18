# NOTE: OSH fails on descriptor 9, but not descriptor 8?  Is this because of
# the Python VM?  How  to inspect state?
read_from_fd.py 8  8<<EOF
here doc on descriptor
EOF
