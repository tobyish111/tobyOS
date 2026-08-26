case $SH in dash|ash) exit ;; esac

py-repr() {
  python2 -c 'import sys; print repr(sys.argv[1])'  "$@"
}

py-repr $'\U0010ffff'
py-repr $(echo -e '\U0010ffff')
py-repr $(printf '\U0010ffff')



# Unicode replacement char 
