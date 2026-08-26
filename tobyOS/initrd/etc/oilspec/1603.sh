shopt -s extglob
mkdir -p _noglob
cd _noglob

set -o noglob
echo @(*)
echo @(__nope__*|__nope__?|'*'|'?'|'[:alpha:]'|'|')
