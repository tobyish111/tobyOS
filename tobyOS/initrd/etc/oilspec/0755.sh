foo() { echo foo; }
wrapper=foo
complete -o default -o nospace -F $wrapper git
