f() {
  read head << EOF
ref: refs/heads/dev/andy
EOF
}
f
echo $head
