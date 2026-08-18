shopt -s extglob
mkdir -p eg5
cd eg5
touch __{aa,'<>','{}','#','&&'}
argv.py @(__aa|'__<>'|__{}|__#|__&&|)

# mksh sorts them differently
