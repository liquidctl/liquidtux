#!/usr/bin/env bash

# TODO: doesn't handle docs. Either:
# a) docs should be moved to repository root. Then -p3 should handle them too.
# b) or patch files will need to be preprocessed

# If git am fails with a message like this:
#
#     error: sha1 information is lacking or useless (nzxt-smart2.c).
#     error: could not build fake ancestor
#
# It means that the patch doesn't apply cleanly, git tries to do a 3-way merge,
# but fails to find blobs mentioned in the patch. This can be fixed by:
# a) fixing the file to match the kernel tree exactly. Maybe some previous
# commit is missing?
# b) adding kernel repository as a remote and fetching it
# c) reducing context lines - i. e. passing -C1

exec git am \
    --include=nzxt-kraken2.c \
    --include=nzxt-smart2.c \
    -p3 \
    "$@"
