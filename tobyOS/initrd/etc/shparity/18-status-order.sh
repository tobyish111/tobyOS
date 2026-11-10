# GAP 4: `$?` must reflect the command that just ran, on the SAME line.
# The shell expanded whole lines before running any of them, so `false; echo $?`
# reported the status of the previous line. Every check below puts the producer
# and the consumer of $? on one line, which is where the bug lived.
false
echo "sep-line=$?"
true;  echo "true=$?"
false; echo "false=$?"
true; false; echo "last-wins=$?"
false; true; echo "recovered=$?"

false || echo "or-ran"
echo "after-or=$?"
true && echo "and-ran"
echo "after-and=$?"
false && echo "never"
echo "after-shortcircuit=$?"
false && echo "no" && echo "also-no"
echo "chain=$?"
true && false || echo "recovering"

if ! false; then echo "negated"; fi
echo "after-if=$?"

# Command substitution must run in order too: this used to be evaluated
# before the assignment on its left.
x=first; y=$(echo "$x"); echo "order=[$y]"
echo status-done
