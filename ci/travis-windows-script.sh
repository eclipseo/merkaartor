#!/bin/bash -x

set -e

git describe --tags --dirty
git status

mkdir build && cd build
cmake .. -G"Unix Makefiles"
make -j4
make VERBOSE=1 package

#TODO: lrelease src/src.pro

#TODO: sh windows/copydeps.sh
#TODO: makensis.exe windows/installer.nsi
#windows/upload-to-bintray.pl windows/merkaartor-*.exe

git describe --tags --dirty
git status

# Prepare deployment description
VERSION=`git describe --tags --dirty`
cat > deploy.json <<EOF
{
    "package": {
        "name": "Merkaartor",
        "repo": "nightly",
        "subject": "krakonos"
    },

    "version": {
        "name": "$VERSION"
    },

    "files":
        [
        {"includePattern": "merkaartor-*.exe", "uploadPattern":"\$1"}
        ],
    "publish": true
}
EOF
