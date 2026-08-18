cd $REPO_ROOT

# https://github.com/pypa/virtualenv/blob/master/virtualenv_embedded/activate.sh
# https://github.com/akinomyoga/ble.sh/blob/6f6c2e5/ble.pp#L374

argv.py "$BASH_SOURCE"  # SimpleVarSub
argv.py "${BASH_SOURCE}"  # BracedVarSub
argv.py "$BASH_LINENO"  # SimpleVarSub
argv.py "${BASH_LINENO}"  # BracedVarSub
argv.py "$FUNCNAME"  # SimpleVarSub
argv.py "${FUNCNAME}"  # BracedVarSub
echo __
source spec/testdata/bash-source-string.sh
