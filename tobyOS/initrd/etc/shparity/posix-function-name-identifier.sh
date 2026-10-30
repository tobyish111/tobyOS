# In posix mode a function's name has to be a valid identifier: `function
# !! ()` is "`!!': not a valid identifier" and a non-interactive shell ends,
# status 2; outside posix mode the name is fine (finding 423).
exec 2>&1
"$BASH" -c 'function !! () { echo hi; }; !!' bash; echo "rc=$?"
"$BASH" -o posix -c 'function !! () { echo hi; }
echo after' bash; echo "rc=$?"
"$BASH" -o posix -c 'f-x() { echo y; }
echo after' bash; echo "rc=$?"
