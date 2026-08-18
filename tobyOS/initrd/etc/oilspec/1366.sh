# Why does OSH on the CI machine behave differently?  Probably a libc bug fix
# I'd guess?

sum=0

# note: NUL byte crashes OSH!
for i in $(seq 1 255); do
  hex=$(printf '%x' "$i")
  c="$(printf "\\x$hex")"  # command sub quirk: \n or \x0a turns into empty string

  #echo -n $c | od -A n -t x1
  #echo ${#c}

  case "$c" in
    # Newline matches empty string somehow.  All shells agree.  I guess
    # fnmatch() ignores trailing newline?
    #'')   echo "[empty i=$i hex=$hex c=$c]" ;;
    "$c") sum=$(( sum + 1 )) ;;
    *)   echo "[bug i=$i hex=$hex c=$c]" ;;
  esac
done

echo sum=$sum
