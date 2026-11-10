# POSIX set -o: ignoreeof and nolog must be ACCEPTED by a script even though
# neither is observable outside an interactive session (bash's own man page
# says nolog is currently ignored; ignoreeof only matters at an interactive
# EOF). tsh rejected both with "unknown option". The greps pin bash's exact
# line format for the option, without pinning the SIZE of the table -- tsh
# carries fewer options than bash and the grep filters to the shared ones.
set -o ignoreeof; echo "1=$?"
set +o ignoreeof; echo "2=$?"
set -o nolog;     echo "3=$?"
set +o nolog;     echo "4=$?"
( set -o ignoreeof; set +o | grep ignoreeof )
( set +o | grep ignoreeof )
( set -o nolog; set -o | grep nolog )
( set -o | grep nolog )
( set -o ignoreeof; set -o | grep ignoreeof )
echo end
