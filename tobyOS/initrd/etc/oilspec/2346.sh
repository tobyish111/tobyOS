# key point:
# unset looks up the stack
# local doesn't though

x=42
unset x
echo x=$x

echo ---

x=42
tmp= unset x
echo x=$x

x=42
tmp= eval 'unset x'
echo x=$x

echo ---

shadow() {
  x=42
  x=tmp unset x
  echo x=$x
  
  x=42
  x=tmp eval 'unset x'
  echo x=$x
}

shadow

echo ---

case $SH in
  bash) set -o posix ;;
esac
shadow

# Now shadow

# unset is a special builtin
# type unset
