if [[ 'foo()' == *\(\) ]]; then echo match1; fi
if [[ 'foo()' == *'()' ]]; then echo match2; fi
if [[ 'foo()' == '*()' ]]; then echo match3; fi

shopt -s extglob

if [[ 'foo()' == *\(\) ]]; then echo match1; fi
if [[ 'foo()' == *'()' ]]; then echo match2; fi
if [[ 'foo()' == '*()' ]]; then echo match3; fi
