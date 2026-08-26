# https://www.utf8-chartable.de/unicode-utf8-table.pl

x=$'\u00C0\u00C8'  # upper grave
y=$'\u00E1\u00E9'  # lower acute

echo u ${x^}
echo U ${x^^}

echo l ${x,}
echo L ${x,,}

echo u ${y^}
echo U ${y^^}

echo l ${y,}
echo L ${y,,}
