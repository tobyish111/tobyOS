argv.py "$(printf 'a\tb')"
argv.py "$(printf '\xE2\x98\xA0')"
argv.py "$(printf '\044e')"
argv.py "$(printf '\0377')"  # out of range
