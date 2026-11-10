# In `case esac in foo|esac) ;; ... esac` the word esac and the pattern esac
# after | do not close the case, and the | is the pattern's, not a pipe --
# tsh cut the line there into a pipeline (finding 433).
exec 2>&1
case esac in foo|esac) ;; *) echo bad1 ;; esac; echo "r=$?"
case esac in foo|esac) echo good1 ;; *) echo bad2 ;; esac
case x in a|esac|x) echo good2 ;; esac | tr g G
case esac in (foo|esac) ;; *) echo bad3 ;; esac; echo "r=$?"
