# mksh and ksh agree this is an esacpe

case $SH in dash|ash) exit ;; esac

show_bytes() {
  # -A n - no file offset
  od -A n -t c -t x1
}

# this isn't special
# mksh doesn't like it
echo -n $'\c'' | show_bytes
