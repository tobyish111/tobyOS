# INT AND QUIT IN AND UNDER AN ASYNCHRONOUS CHILD (bash's
# setup_async_signals and restore_original_signals): run without job
# control, the async command itself ignores both -- a simple command, each
# element of a pipeline -- but what an async { } or ( ) runs inside gets
# them back, and the child may trap INT or put it back to the default
# (POSIX interpretation 751). tsh handed its ignore to everything inside,
# and `trap - INT` left it ignored (logs/b15 p2, p3 a1-a10). SigIgn is the
# guest's /proc view, the same for both shells.
exec 2>&1
sed -n 's/^SigIgn:[[:space:]]*/a1-fg /p' /proc/self/status
sed -n 's/^SigIgn:[[:space:]]*/a2-async /p' /proc/self/status & wait
{ sed -n 's/^SigIgn:[[:space:]]*/a3-brace-last /p' /proc/self/status; } & wait
{ sed -n 's/^SigIgn:[[:space:]]*/a4-brace-mid /p' /proc/self/status; :; } & wait
( sed -n 's/^SigIgn:[[:space:]]*/a5-paren /p' /proc/self/status ) & wait
sed -n 's/^SigIgn:[[:space:]]*/a6-pipe /p' /proc/self/status | cat & wait
{ trap - INT; sed -n 's/^SigIgn:[[:space:]]*/a7-revert-int /p' /proc/self/status; } & wait
{ trap - QUIT; sed -n 's/^SigIgn:[[:space:]]*/a8-revert-quit /p' /proc/self/status; } & wait
{ trap 'echo x' INT; sed -n 's/^SigIgn:[[:space:]]*/a9-trapped-int /p' /proc/self/status; } & wait
{ trap - INT; trap -p INT; kill -INT $BASHPID; echo a10-after; } & wait
{ trap 'echo v1-got-int' INT; kill -INT $BASHPID; echo v1-after; } & wait
{ kill -INT $BASHPID; kill -QUIT $BASHPID; echo v2-ignored; } & wait
echo end
