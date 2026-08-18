# This is not that important -- see core/sh_init.py
# Instead of verifying that stat('.') == stat(PWD), which is two sycalls,
# OSH just calls getcwd() unconditionally.

# so C++ leak sanitizer  doesn't print to stderr
export ASAN_OPTIONS='detect_leaks=0'

strace -e getcwd -- $SH -c 'echo hi; pwd; echo $PWD' 1> /dev/null 2> err.txt

wc -l err.txt
#cat err.txt
