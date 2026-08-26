# https://www.gnu.org/software/bash/manual/html_node/Shell-Parameter-Expansion.html

x='abc DEF'

echo "${x@u}"

echo "${x@U}"

echo "${x@L}"
