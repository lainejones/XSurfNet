#!/bin/sh
# dist.sh - stage the release package in dist/XSurfNet (+ XSurfNet.info).
# Build first (make); the .lha is made on an Amiga from this tree.
set -e
cd "$(dirname "$0")"
for f in out/xsurfnet.device out/xsstat out/xsflush; do
    [ -f "$f" ] || { echo "missing $f - run make" >&2; exit 1; }
done
rm -rf dist
mkdir -p dist/XSurfNet/Devs/Networks dist/XSurfNet/Tools
cp out/xsurfnet.device dist/XSurfNet/Devs/Networks/
cp out/xsstat out/xsflush dist/XSurfNet/Tools/
cp README.md dist/XSurfNet/README
cp LICENSE dist/XSurfNet/LICENSE
cp icons/README.info dist/XSurfNet/README.info
cp icons/drawer.info dist/XSurfNet.info
cp icons/drawer.info dist/XSurfNet/Devs.info
cp icons/drawer.info dist/XSurfNet/Tools.info
cp icons/drawer.info dist/XSurfNet/Devs/Networks.info
find dist -type f | sort
