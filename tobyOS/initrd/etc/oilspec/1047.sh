printf '%d\n' '64#a'
echo status=$?

# bash, dash, and mksh print 64 and return status 1
# zsh and ash print 0 and return status 1
# OSH rejects it completely (prints nothing) and returns status 1
