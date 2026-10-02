# `function NAME` with or without the parentheses, the body on the next line,
# any word as the name: bash's other spelling of a function definition. tsh
# knew only `NAME()` -- `function f1 { ...; }` ran a command called function.
exec 2>&1
function f1 { echo one; }; f1
function f2() { echo two; }; f2
function f3 {
  echo three
}
f3
function f4
{ echo four; }
f4
function f5 ( ) { echo five; }; f5
function f-6 { echo six; }; f-6
function f7 { echo seven; } > /dev/null; f7; echo r7=$?
echo end
