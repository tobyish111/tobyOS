case $SH in dash) exit ;; esac

test-hyphen() {
  echo "a   : '${a-no-colon}' '${a:-with-colon}'"
  echo "a[0]: '${a[0]-no-colon}' '${a[0]:-with-colon}'"
}

a=()
test-hyphen
a=("")
test-hyphen
a=("" "")
test-hyphen
IFS=
test-hyphen


# Zsh's ${a} and ${a[@]} implement something different from the other shells'.



# Bash 2.0..4.4 has a bug that "${a[@]:-xxx}" produces an empty string.  It
# seemed to consider a[@] and a[*] are non-empty when there is at least one
# element even if the element is empty.  This was fixed in Bash 5.0.
#
# ## BUG bash STDOUT:
# a[@]: 'no-colon' 'with-colon'
# a[*]: 'no-colon' 'with-colon'
# a[@]: '' ''
# a[*]: '' ''
# a[@]: ' ' ' '
# a[*]: ' ' ' '
# a[@]: ' ' ' '
# a[*]: '' ''
# ## END

# Zsh's ${a} and ${a[@]} implement something different from the other shells'.
