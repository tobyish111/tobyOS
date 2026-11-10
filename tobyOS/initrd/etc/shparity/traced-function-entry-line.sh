# The DEBUG trap a traced function runs as its body starts is on the
# body's first line, its `{` -- tsh gave the line of the call (finding
# 513).
exec 2>&1
trap 'echo ${FUNCNAME:-main}[$LINENO] debug' DEBUG
func2()
{
	echo body
}
declare -ft func2
func2
trap - DEBUG
