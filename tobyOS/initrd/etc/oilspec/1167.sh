eval() {
  echo 'eval func' "$@"
}
eval 'echo hi'

# we allow redefinition, but the definition is NOT used!

# we PREVENT redefinition

# should not allow redefinition
