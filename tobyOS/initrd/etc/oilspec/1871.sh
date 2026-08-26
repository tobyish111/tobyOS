orig() {
  export NIX_LDFLAGS${role_post}+=" -L$1/lib64"
}

new() {
  local var_name="NIX_LDFLAGS$role_post"
  local value=" -L$1/lib64"

  eval "$var_name"+='$value'
  export "$var_name"
}

role_post='_foo'

# set -u

if test -n "${BASH_VERSION:-}"; then
  orig one
fi

declare -p NIX_LDFLAGS_foo  # inspect it
unset NIX_LDFLAGS_foo

new one

declare -p NIX_LDFLAGS_foo  # inspect it
