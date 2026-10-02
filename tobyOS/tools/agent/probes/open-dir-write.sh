# open(2) of a DIRECTORY for writing is EISDIR on Linux (may_open), so a
# redirection onto one fails at the open and the command never runs; a
# read-only open succeeds and the READ fails. Run under bash: the kernel is
# what is being measured.
echo x > /tmp; echo "s1 $?"
echo x >> /tmp; echo "s2 $?"
: <> /tmp; echo "s3 $?"
cat < /tmp; echo "s4 $?"
exec 3< /tmp && echo "s5 opened"; exec 3<&-
