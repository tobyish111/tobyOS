# test/spec-common.sh sets LC_ALL=C.UTF_8
unset LC_ALL

touch hello hello.py hello_preamble.sh hello-test.sh
echo h*

# bash - hello_preamble.h comes first
# But ord('_') == 95 
#     ord('-') == 45

# https://serverfault.com/questions/122737/in-bash-are-wildcard-expansions-guaranteed-to-be-in-order

#LC_COLLATE=C.UTF-8
LC_COLLATE=en_US.UTF-8  # en_US is necessary
echo h*

LC_COLLATE=en_US.UTF-8 $SH -c 'echo h*'
