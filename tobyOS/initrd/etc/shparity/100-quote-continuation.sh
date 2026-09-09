# The quoting and line-continuation constructs behind the Oils
# builtin-type failure (1286), isolated from it.
#
# That case defines a normalize() helper whose body is a backslash-continued
# `sed` with several -e arguments, and which builds its character class from
#
#     quotes='"`'\'
#
# -- a single-quoted string holding a double quote and a BACKTICK, followed
# by an escaped single quote. Under tsh the helper ended up handing sed an
# argument of `;`, which sed reported as a missing file. Nothing in the
# helper contains a semicolon, so something in the parse is inventing one.
#
# Each line below isolates one candidate. Whichever diverges from bash is
# the bug; the rest are here so the answer is unambiguous in one run.

# 1. A BACKTICK INSIDE SINGLE QUOTES is data. If it opened a command
#    substitution the rest of the line would be swallowed.
q1='"`'
echo "sq-backtick=[$q1] len=${#q1}"

# 2. The exact idiom: single-quoted pair, then an escaped single quote
#    glued on, giving three characters.
quotes='"`'\'
echo "three=[$quotes] len=${#quotes}"

# 3. A backslash-continued command with plain arguments.
printf '%s|' \
  one \
  two \
  three
echo

# 4. The same, with arguments that carry the quoting from (2).
printf '%s|' \
  -e "s/[$quotes]//g" \
  -e 's/shell function/function/'
echo

# 5. All of it inside a function body, which is how the case meets it.
norm() {
  local inner='"`'\'
  printf '%s|' \
    -e "s/[$inner]//g" \
    -e 's/a/b/'
  echo
}
norm

# 6. A backtick in a double-quoted string IS substitution, and must stay so.
echo "dq-backtick=[`echo sub`]"

# 7. A comment line inside a continued command's neighbourhood.
printf '%s|' one \
  two          # trailing comment
echo
echo end
