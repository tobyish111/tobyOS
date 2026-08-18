mkdir -p _tmp
touch '_tmp/[bc]ar.mm' # file that looks like a glob pattern
touch _tmp/bar.mm _tmp/car.mm
argv.py '_tmp/[bc]'*.mm - _tmp/?ar.mm
