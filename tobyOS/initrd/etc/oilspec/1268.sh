touch _tmp/{mv,tar,grep}
chmod +x _tmp/{mv,tar,grep}
PATH=_tmp:$PATH

mv () { ls; }
tar () { ls; }
grep () { ls; }
type -P mv tar grep cd builtin command type
