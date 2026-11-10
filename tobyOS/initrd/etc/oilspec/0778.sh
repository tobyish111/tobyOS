# YSH has a superset of keywords:
# const var
# setvar setglobal
# proc func typed
# call =   # hm = is not here

compgen -k | sort | egrep '^(const|var|setvar|setglobal|proc|func|typed|call|=)$'
echo --
