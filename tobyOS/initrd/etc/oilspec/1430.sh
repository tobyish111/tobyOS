# mksh doesn't have this syntax of regex matching.  I guess it comes from perl?
regex='.*\.py'
[[ foo.py =~ $regex ]] && echo true
[[ foo.p  =~ $regex ]] || echo false
