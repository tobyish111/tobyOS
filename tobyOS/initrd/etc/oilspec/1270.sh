touch /tmp/{mv,tar,grep}
chmod +x /tmp/{mv,tar,grep}
PATH=/tmp:$PATH

mv () { ls; }
tar () { ls; }
grep () { ls; }
type -f mv tar grep
