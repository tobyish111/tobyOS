echo shell
small=$'\u00DF'
echo u ${small^}
echo U ${small^^}

echo l ${small,}
echo L ${small,,}
echo

echo python2
python2 -c '
small = u"\u00DF"
print(small.upper().encode("utf-8"))
print(small.lower().encode("utf-8"))
'
echo

# Not in the container images, but python 3 DOES support it!
# This is moved to demo/survey-case-fold.sh

if false; then
echo python3
python3 -c '
import sys
small = u"\u00DF"
sys.stdout.buffer.write(small.upper().encode("utf-8") + b"\n")
sys.stdout.buffer.write(small.lower().encode("utf-8") + b"\n")
'
fi

if false; then
  # Yes, supported
  echo node.js

  nodejs -e '
  var small = "\u00DF"
  console.log(small.toUpperCase())
  console.log(small.toLowerCase())
  '
fi
