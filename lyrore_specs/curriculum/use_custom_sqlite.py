#!/usr/bin/env python3
"""
Custom SQLite Wrapper

Allows using a custom-compiled SQLite library (e.g., with Lyrore patches)
by setting LD_PRELOAD before calling use_sqlite.py.

Usage:
    python use_custom_sqlite.py --custom_lib /path/to/libsqlite3.so <use_sqlite.py args>

Example:
    python use_custom_sqlite.py --custom_lib /tmp/custom_sqlite/lib/libsqlite3.so \
        load --config db_config.json
"""

import argparse
import os
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).parent.resolve()


def main():
    parser = argparse.ArgumentParser(
        description="Run use_sqlite.py with a custom SQLite library",
        formatter_class=argparse.RawDescriptionHelpFormatter
    )

    parser.add_argument('--custom_lib', required=True,
                        help='Path to custom libsqlite3.so')
    parser.add_argument('--custom_extension', 
                        help='Path to custom curriculum_udfs.so (optional)')

    # Parse known args, pass rest to use_sqlite.py
    args, remaining = parser.parse_known_args()

    # Validate custom library exists
    if not os.path.exists(args.custom_lib):
        print(f"ERROR: Custom library not found: {args.custom_lib}", file=sys.stderr)
        sys.exit(1)

    # Set up environment
    env = os.environ.copy()

    # Use LD_PRELOAD to load custom SQLite library
    existing_preload = env.get('LD_PRELOAD', '')
    if existing_preload:
        env['LD_PRELOAD'] = f"{args.custom_lib}:{existing_preload}"
    else:
        env['LD_PRELOAD'] = args.custom_lib

    # Also set LD_LIBRARY_PATH for the directory containing the library
    lib_dir = os.path.dirname(os.path.abspath(args.custom_lib))
    existing_lib_path = env.get('LD_LIBRARY_PATH', '')
    if existing_lib_path:
        env['LD_LIBRARY_PATH'] = f"{lib_dir}:{existing_lib_path}"
    else:
        env['LD_LIBRARY_PATH'] = lib_dir

    # If custom extension specified, update config
    if args.custom_extension:
        env['CURRICULUM_EXTENSION_PATH'] = args.custom_extension

    # Build command
    use_sqlite_path = SCRIPT_DIR / 'use_sqlite.py'
    cmd = [sys.executable, str(use_sqlite_path)] + remaining

    print(f"Using custom SQLite: {args.custom_lib}")
    if args.custom_extension:
        print(f"Using custom extension: {args.custom_extension}")
    print(f"Running: {' '.join(cmd)}")
    print()

    # Execute
    result = subprocess.run(cmd, env=env)
    sys.exit(result.returncode)


if __name__ == '__main__':
    main()
