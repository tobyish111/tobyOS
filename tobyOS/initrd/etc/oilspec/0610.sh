echo -{a,b}{1...3}-
echo -{a,{1...3}}-
echo {a,b}{}
# osh doesn't expand ANYTHING on invalid syntax.  That's OK because of the test
# case below.
