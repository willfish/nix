#!/usr/bin/env bash

if [ -z "$1" ]; then
  echo "Usage: $0 <interpreter>"
  exit 1
fi

if [ -d .nix/ ]; then
  dll=$(find .nix/ -type f -name 'sorbet' | grep libexec)
  old_interpreter=$(patchelf --print-interpreter $dll)

  echo "Old interpreter: $old_interpreter"
  echo "New interpreter: $1"
  echo "Setting new interpreter..."

  sudo patchelf --set-interpreter $1 $dll
fi
