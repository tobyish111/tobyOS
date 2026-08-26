# 2025-03: What's the cause of this bug?
#
# OSH is very wrong here
#   ['a', '\\', 'b']
# Is this a fundamental problem with the IFS state machine?
# It definitely relates to the use of backslashes.
# So we have at least 4 backslash bugs

IFS=':'
word='a:'
argv.py ${word}:b
argv.py ${word}:

echo ---

# Same thing happens for 'z'
IFS='z'
word='az'
argv.py ${word}zb
argv.py ${word}z
