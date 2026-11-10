# caller takes no options -- "-X: invalid option" -- and N is a number as
# bash reads one, blanks and a sign allowed, else "invalid number" (octal,
# hex); either way the usage line, status 2. tsh said "invalid frame
# number", status 1, to all of them (finding 532).
exec 2>&1
f() {
    caller foo; echo "rc=$?"
    caller -1; echo "rc=$?"
    caller -x; echo "rc=$?"
    caller 0x1; echo "rc=$?"
    caller 08x; echo "rc=$?"
    caller ''; echo "rc=$?"
    caller -- 5; echo "rc=$?"
    caller ' 5 '; echo "rc=$?"
    caller +5; echo "rc=$?"
}
f
