# In posix mode `return` outside a function or a sourced file is an error
# of a special builtin, which ends a non-interactive shell with status 2;
# outside posix mode the shell goes on (finding 422).
exec 2>&1
"$BASH" -o posix -c 'return; echo after return' bash; echo "rc=$?"
"$BASH" -c 'return; echo after return' bash; echo "rc=$?"
"$BASH" -o posix -c 'f() { return 3; }; f; echo "f=$?"' bash; echo "rc=$?"
