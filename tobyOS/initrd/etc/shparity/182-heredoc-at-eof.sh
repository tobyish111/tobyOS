# A here-document that the end of the input cuts short is still a
# here-document: bash warns ("here-document at line N delimited by
# end-of-file (wanted `EOF')", on stderr) and runs the command with the
# lines it got. tsh reported an error and ran nothing, status 2. The cut-off
# documents are in eval and sh -c texts, so this file itself goes on.
eval 'cat <<EOF
one
two'
echo "eval: $?"
eval 'cat <<-X
	tabbed'
echo "eval strip: $?"
eval 'cat <<"Q"
$not_expanded'
echo "eval quoted: $?"
eval 'cat <<EOF'
echo "eval empty: $?"
eval 'cat <<A; cat <<B
a1
A
b1'
echo "eval two: $?"
echo done
