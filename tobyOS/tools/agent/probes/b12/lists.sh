for a in builtin keyword setopt shopt signal helptopic binding variable enabled disabled export function alias arrayvar user group hostname service; do
  printf '== %s: ' "$a"; compgen -A $a | tr '\n' '|'; echo
done
echo "== bindlines"; bind -p 2>&1 | head -5
echo "== V"; bind -V 2>&1 | head -3
echo "== versions"; echo "$BASH_VERSION"
