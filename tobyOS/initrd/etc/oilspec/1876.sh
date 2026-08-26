shopt -s nocasematch
case a in A) echo 0 ;; *) echo 1 ;; esac
case A in a) echo 0 ;; *) echo 1 ;; esac
case a in [A]) echo 0 ;; *) echo 1 ;; esac
case A in [a]) echo 0 ;; *) echo 1 ;; esac
