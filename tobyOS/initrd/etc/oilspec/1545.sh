# AST has extra Sentence nodes; there was a REGRESSION here.
set -o errexit; echo one; ! true; echo two; ! false; echo three
