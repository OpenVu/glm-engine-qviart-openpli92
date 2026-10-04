#!/usr/bin/env bash
set -euo pipefail

OPENPLI_REF="${OPENPLI_REF:-release-9.2}"
ROOT="$(cd "$(dirname "$0")" && pwd)"
OE="$ROOT/openpli-oe-core"

if [[ ! -d "$OE/.git" ]]; then
  git clone --depth 1 --branch "$OPENPLI_REF" --recurse-submodules --shallow-submodules \
    https://github.com/OpenPLi/openpli-oe-core.git "$OE"
fi

cd "$OE"
make init
rm -rf meta-glm
cp -a "$ROOT/meta-glm" "$OE/meta-glm"

if ! grep -q 'meta-glm' build/conf/bblayers.conf; then
  printf '\nBBLAYERS += "${TOPDIR}/../meta-glm"\n' >> build/conf/bblayers.conf
fi

. build/env.source
MACHINE=dual bitbake glm-engine

find build/tmp/deploy -type f -name 'glm-engine_*.ipk' -print
