# oil 0.8.pre4 does not fail with non-existent fd 100.
fd=100
echo foo53 >&$fd
