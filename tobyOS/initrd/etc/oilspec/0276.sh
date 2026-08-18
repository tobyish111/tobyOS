case $SH in mksh) exit ;; esac

case $SH in
  bash)
    # disabled with soil-ovm-tarball image 2025-04-30b - the CI runs on Debian 12
    # now
    exit

    # Work around bash integer overflow bug that only happens on say Debian 10,
    # but NOT Debian 12.  The bug exists in bash 5.2.  It's unclear why it
    # depends on the OS version.
    v='/etc/debian_version'
    # debian version 10 / debian buster
    if test -f $v && grep -E 'buster/sid|^10' $v >/dev/null; then
      cat << 'EOF'
[x][x]
[y x][y x]
[z y x][z y x]
[z y x][z y x]
EOF
      exit
    fi
    # Actual STDOUT of buggy bash builds:
    # [][]
    # [][]
    # [][]
    # [][]
    ;;
esac

a=(1 2 3)
a[0x7FFFFFFFFFFFFFFF]=x
a[0x7FFFFFFFFFFFFFFE]=y
a[0x7FFFFFFFFFFFFFFD]=z

echo "[${a[@]: -1}][${a[*]: -1}]"
echo "[${a[@]: -2}][${a[*]: -2}]"
echo "[${a[@]: -3}][${a[*]: -3}]"
echo "[${a[@]: -4}][${a[*]: -4}]"
