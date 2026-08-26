export e=E
printenv.py e
typeset +x e=E2
printenv.py e  # no longer exported
