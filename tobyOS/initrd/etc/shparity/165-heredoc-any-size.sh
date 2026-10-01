# Here-documents of ANY size, with a delimiter of ANY length (S7). tsh capped
# a body at one pipe buffer (64 KiB) -- a 70 KB document made the whole
# script print nothing and exit 2 -- and a delimiter at 63 characters: a
# longer one was "body missing" and the body and the delimiter then RAN AS
# COMMANDS. The big document and the long delimiter are built here and fed
# through eval, which reads here-documents the same way a script does.
big=$(i=0; while [ $i -lt 2000 ]; do echo "line $i: ...................................................................................................."; i=$((i+1)); done)
echo "built ${#big}"
eval "cat <<EOF | wc -c
$big
EOF"
eval "cat <<'EOF' | md5sum
$big
EOF"
eval "x=\$(cat <<EOF
$big
EOF
); echo \"captured \${#x}\""
d=$(printf 'D%.0s' $(seq 1000))
eval "cat <<$d
body-line
$d
echo after-the-long-delimiter"
cat <<-EOF
	tab-stripped
		two tabs, one left
	EOF
cat <<'EOF'
$HOME is not expanded
EOF
cat <<A; cat <<B
first
A
second
B
echo done
