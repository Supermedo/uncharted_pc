#!/usr/bin/env bash
# Starts the Uncharted launcher (GTK4). Needs Python, PyGObject, GTK 4 and libadwaita.
here=$(cd -- "$(dirname -- "$0")" && pwd)
exec python3 "$here/uncharted_launcher_gtk.py" "$@"
