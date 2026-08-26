shopt -s extglob
mkdir -p eg7
cd eg7
touch '_[:]' '_*' '_?'
argv.py @('_[:]'|'_*'|'_?')
argv.py @(nested|'_?'|@('_[:]'|'_*'))

# mksh sorts them differently
