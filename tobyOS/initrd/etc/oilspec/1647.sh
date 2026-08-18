# dash allows this, but bash does not.  The POSIX grammar might not allow
# this?  Because a function body needs a compound command.
# function_body    : compound_command
#                  | compound_command redirect_list  /* Apply rule 9 */
