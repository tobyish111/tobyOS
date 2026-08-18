shopt -s expand_aliases  # bash requires this
alias hi='echo hello world'
hi || echo 'should not run this'
echo hi  # second word is not
'hi' || echo 'expected failure'
