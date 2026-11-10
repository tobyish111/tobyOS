[[ 'x' =~ \. ]] || echo false
[[ '.' =~ \. ]] && echo true

[[ 'xx' =~ \^\$ ]] || echo false
[[ '^$' =~ \^\$ ]] && echo true

[[ 'xxx' =~ \+\*\? ]] || echo false
[[ '*+?' =~ \*\+\? ]] && echo true

[[ 'xx' =~ \{\} ]] || echo false
[[ '{}' =~ \{\} ]] && echo true
