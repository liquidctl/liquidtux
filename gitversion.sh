#!/usr/bin/env bash

set -e

cd -- "$( dirname -- "${BASH_SOURCE[0]}" )"

# Should be replaced with 'git describe' output by git-archive.
# Unfortunately, '--long' isn't supported.
ARCHIVE_VERSION='$Format:%(describe:abbrev=7)$'

if [[ "$ARCHIVE_VERSION" == "$"* ]]; then
    # ARCHIVE_VERSION contains the original placeholder - not a git archive
    git describe --long --abbrev=7 | sed -e 's/^v//'
else
    echo "$ARCHIVE_VERSION" | sed -e 's/^v//'
fi
