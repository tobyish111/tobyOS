# mksh and zsh implement splitting with $REPLY, bash/ash don't

echo '  a b  ' | (read; echo "[$REPLY]")
echo '  a b  ' | (read myvar; echo "[$myvar]")

echo '  a b  \
  line2' | (read; echo "[$REPLY]")
echo '  a b  \
  line2' | (read myvar; echo "[$myvar]")

# Now test with -r
echo '  a b  \
  line2' | (read -r; echo "[$REPLY]")
echo '  a b  \
  line2' | (read -r myvar; echo "[$myvar]")
