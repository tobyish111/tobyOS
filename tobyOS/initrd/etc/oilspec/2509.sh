s='abcd'
argv.py "${s%%abcd}" "${s%abcd}" "${s#abcd}" "${s##abcd}"
# failure case:
argv.py "${s%%abcde}" "${s%abcde}" "${s#abcde}" "${s##abcde}"
