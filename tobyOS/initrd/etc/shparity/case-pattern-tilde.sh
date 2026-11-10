# A tilde starting a case pattern expands, its home literal in the
# pattern: `case $SHELL in ~/bash)` matches $HOME/bash -- tsh compared the
# `~` (finding 500).
exec 2>&1
HOME=/usr/x*z
SHELL=/usr/x*z/bash
case $SHELL in ~/bash) echo ok1;; *) echo bad1;; esac
case /usr/xyz/bash in ~/bash) echo bad2;; *) echo ok2;; esac
case "~/bash" in ~/bash) echo bad3;; \~/bash) echo ok3;; esac
