#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Launch the local dashboard.  If a server is already listening on the
# default port, open the browser instead of starting a second instance.

URL="http://127.0.0.1:8722/"

if [ "$#" -eq 0 ] || [ "$1" = "--open" ]; then
    if command -v curl >/dev/null 2>&1 &&
       curl -fsS --max-time 1 "${URL}api/health" >/dev/null 2>&1; then
        if command -v xdg-open >/dev/null 2>&1; then
            xdg-open "$URL" >/dev/null 2>&1 &
        fi
        exit 0
    fi
fi

exec python3 /usr/local/lib/alg-rgb/web/app.py "$@"
