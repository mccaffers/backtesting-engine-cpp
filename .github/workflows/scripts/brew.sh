#!/bin/bash

# Function to check if a package is already installed via Homebrew, then skip installation if it is
check_and_install() {
    if ! brew list $1 &>/dev/null; then
        echo "Installing $1..."  # Inform the user that the package is being installed
        brew install $1  # Install the package using Homebrew
    else
        echo "$1 is already installed"  # Inform the user that the package is already installed
    fi
}

# Install packages if they don't exist
check_and_install postgresql@18 # Check and install PostgreSQL (which includes libpq)
check_and_install pkg-config  # Check and install pkg-config