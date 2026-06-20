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
check_and_install boost # Check and install Boost
check_and_install llvm  # Clang + libc++ with the `std` module (import std)
check_and_install ninja # Ninja generator (required for C++23 modules / import std)
check_and_install libomp # OpenMP runtime (linked by the engine library)