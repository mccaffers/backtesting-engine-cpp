#!/bin/bash

# Check if a package is already installed via Homebrew, then skip
check_and_install() {
    if ! brew list $1 &>/dev/null; then
        echo "Installing $1..."
        brew install $1
    else
        echo "$1 is already installed"
    fi
}

# Install packages if they don't exist
check_and_install postgresql
check_and_install pkg-config