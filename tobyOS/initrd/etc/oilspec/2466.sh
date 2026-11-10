export LC_ALL='C'

s='_μ_ and _μ_'

# ? should match one char

echo ${s//_?_/foo}  # all
echo ${s/#_?_/foo}  # left
echo ${s/%_?_/foo}  # right
echo

a='_x_ and _y_'

echo ${a//_?_/foo}  # all
echo ${a/#_?_/foo}  # left
echo ${a/%_?_/foo}  # right
