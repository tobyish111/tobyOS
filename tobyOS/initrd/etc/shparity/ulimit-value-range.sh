# bash reads a `ulimit` value's digits in the limit's own 64-bit unsigned
# type, wrapping -- 2^64 is 0, 2^64-1 is "unlimited" -- and says "limit out
# of range" only when the value times the block size overflows it; what
# fits goes to the kernel, which may refuse it in its words. tsh refused
# anything past 2^63 over the block size (finding 651).
exec 2>&1
(ulimit -f $((1 << 53)); echo "f 2^53: $? $(ulimit -f)")
(ulimit -f 18014398509481983; echo "f 2^54-1: $? $(ulimit -f)")
(ulimit -f 18014398509481984; echo "f 2^54: $? $(ulimit -f)")
(set -o posix; ulimit -f 18014398509481984; echo "posix f 2^54: $? $(ulimit -f)")
(ulimit -t 9223372036854775808; echo "t 2^63: $? $(ulimit -t)")
(ulimit -t 18446744073709551614; echo "t 2^64-2: $? $(ulimit -t)")
(ulimit -t 18446744073709551615; echo "t 2^64-1: $? $(ulimit -t)")
(ulimit -t 18446744073709551616; echo "t 2^64: $? $(ulimit -t)")
(ulimit -c 9007199254740993; echo "c 2^53+1: $? $(ulimit -c)")
(ulimit -n 9223372036854775808; echo "n 2^63: $? $(ulimit -n)")
