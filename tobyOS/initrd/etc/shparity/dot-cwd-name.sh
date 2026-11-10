# A file `.` finds in the working directory goes by the name it was given:
# BASH_SOURCE and its diagnostics say "x533.sub", where tsh said the whole
# path (finding 533).
exec 2>&1
echo 'echo "${BASH_SOURCE[0]}"; nosuchcmd_533' > x533.sub
. x533.sub
