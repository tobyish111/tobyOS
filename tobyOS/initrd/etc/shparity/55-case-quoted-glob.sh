# A QUOTED GLOB METACHARACTER IN A CASE PATTERN.
#
# Regression test for a General Protection fault: shell_case_unquote()
# rewrote the pattern in place while escaping what was inside the quotes,
# so the write cursor overtook the read cursor after the SECOND
# metacharacter, destroyed the closing quote and the terminator, and grew
# two bytes per byte read until it ran off the top of the stack. The shell
# exited 255 in the middle of the script. `case $f in "*.txt")` is ordinary
# shell, so any script could reach it.
case 'x[a]y' in (*'[a]'*) echo one ;; esac
case 'x[a]y' in *'[a]'*) echo two ;; esac
case 'xay' in (*'[a]'*) echo NOT-REACHED ;; *) echo three ;; esac
case 'a*b' in "a*b") echo four ;; esac
case 'ayb' in "a*b") echo NOT-REACHED ;; *) echo five ;; esac
case 'x[my sed command]y' in (*'[my sed command]'*) echo six ;; esac
case 'q?r' in 'q?r') echo seven ;; esac
case '[[[[' in '[[[[') echo eight ;; esac
echo done
