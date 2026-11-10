#!/usr/bin/env python3
"""Compatibility entry point for installing an Android Qt kit."""

import sys
from install_qt import main

if __name__ == "__main__":
    sys.argv.insert(1, "android")
    main()
