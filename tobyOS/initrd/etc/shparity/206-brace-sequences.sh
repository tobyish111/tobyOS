# Brace sequences: a zero-padded end pads every term to the wider width,
# sign included; a step of 0 is 1; a `{...}` that is neither a list nor a
# range stays as it is while a later group still expands. tsh printed plain
# numbers, refused step 0 and left `{x}_{a,b}` alone.
exec 2>&1
echo {01..3} / {-01..2} / {1..10..03} / {008..10}
echo {1..5..0} {5..1..0} {a..e..2}
echo {x}_{a,b} a{b}c{d,e}
