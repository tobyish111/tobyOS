# A file name's leading period is matched only by a `.` in the pattern --
# inside an extglob group too: @(.foo), @(.foo|*), *(bar).foo take .foo,
# @(?|.?) takes .b and .. but not ., and a !( ) never takes a dotfile; tsh
# kept every dotfile from an extglob pattern (finding 458).
exec 2>&1
shopt -s extglob; d=$(mktemp -d); cd "$d" || exit; touch .foo bar .a
echo @(.foo); echo @(.foo|*); echo *(bar).foo; echo ?(bar).foo; echo !(bar).foo; echo @(*|.!(|.))
shopt -u globskipdots
echo @(.?); echo @(?|.?); echo @(?|.*); echo .*
shopt -s dotglob; echo @(.*); echo !(.*); echo *
cd /; rm -rf "$d"
