set -o errexit
echo hi | grep nonexistent || echo ok
