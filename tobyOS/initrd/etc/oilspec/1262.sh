touch /tmp/{mv,tar,grep}
chmod +x /tmp/{mv,tar,grep}
PATH=/tmp:$PATH

type -p mv tar grep
echo --
type -P mv tar grep
