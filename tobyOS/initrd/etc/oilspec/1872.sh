for i in 0 1 2; do
  echo i=$i

  NIX_ENFORCE_NO_NATIVE=$i

  outputVar=NIX_ENFORCE_NO_NATIVE_x86_64_unknown_linux_gnu
  inputVar=NIX_ENFORCE_NO_NATIVE

  # Original Nix idiom

  if test -n "$BASH_VERSION"; then
    let "${outputVar} |= ${!inputVar:-0}" "1"
  else
    # OSH alternative
    eval ": \$(( ${outputVar} |= ${!inputVar:-0} ))"
  fi

  echo NIX_ENFORCE_NO_NATIVE=$NIX_ENFORCE_NO_NATIVE
  echo NIX_ENFORCE_NO_NATIVE_x86_64_unknown_linux_gnu=$NIX_ENFORCE_NO_NATIVE_x86_64_unknown_linux_gnu
  echo

done
