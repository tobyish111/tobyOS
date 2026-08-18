set -e
shopt -s command_sub_errexit || true

echo before
echo $(exit 42)
echo after
