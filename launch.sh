#!/bin/sh
# Short alias used by install.sh symlink target documentation.
exec "$(CDPATH= cd -- "$(dirname "$0")" && pwd)/run-instead-of-kodi.sh" "$@"
