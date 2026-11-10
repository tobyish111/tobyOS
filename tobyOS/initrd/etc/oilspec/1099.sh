IFS=''
read x y <<EOF
  a b c d
EOF
echo "[$x|$y]"
