# With globstar, `**` as the last component lists every file and directory
# below, at any depth, and the directory itself when something came before
# it -- `lib/**` is lib/ lib/glob lib/glob/g.o -- a pattern ending in `/`
# lists directories, each with its `/`, a link to one too, and dot names
# need dotglob (finding 349). tsh matched nothing, and `**` was a `*`.
shopt -s globstar
cd "${TMPDIR:-/tmp}" && rm -rf gsl.$$ && mkdir gsl.$$ && cd gsl.$$ || exit 1
mkdir -p lib/glob lib/sh b x/y/z .h/i; touch lib/glob/g.o lib/sh/s.o b/h.o top.o x/y/g x/y/z/f .h/i/k
ln -s lib c
echo lib/**; echo **; echo **/*.o; echo lib/**/*.o
echo **/; echo x/**/; echo **/y/**; echo **/l*/
shopt -s dotglob; echo **; echo **/k
cd .. && rm -rf gsl.$$
