# This aborts because it's not part of an if statement.
set -o errexit
{ echo one; false; echo two; }
