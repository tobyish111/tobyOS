# A tilde starting a case word expands: `case ~ in $HOME)` matches -- tsh
# compared the `~` (finding 505). A quoted one stays.
exec 2>&1
HOME=/usr/xyz
case ~ in $HOME) echo ok1;; *) echo bad1;; esac
case ~ in ~) echo ok2;; \~) echo bad2;; esac
case ~/x in /usr/xyz/x) echo ok3;; *) echo bad3;; esac
case "~" in ~) echo bad4;; \~) echo ok4;; esac
