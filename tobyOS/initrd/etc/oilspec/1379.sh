# Hm this subhell has to know about the closing ) and stuff like that.
# case_clause is a compound_command, which is a command.  And a subshell
# takes a compound_list, which is a list of terms, which has and_ors in them
# ... which eventually boils down to a command.
echo $(foo=a; case $foo in [0-9]) echo number;; [a-z]) echo letter ;; esac)
