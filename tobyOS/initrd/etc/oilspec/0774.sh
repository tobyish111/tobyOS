# NOTE: this test isn't hermetic
compgen -A command xarg | uniq
echo status=$?
