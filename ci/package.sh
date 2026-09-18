#!/usr/bin/env bash
# Stages the release archive for one platform: the built external, the help
# patch and the example plugins, flat at the zip root so unzipping it into a
# Pd path (or next to a patch) just works — Pd finds the external and the
# help patch in the patch's own directory.
#
# usage: package.sh <platform-suffix> <external> [extra-file ...]
#
# VERSION names the archive; defaults to the current commit, which is what a
# workflow_dispatch run (no tag) wants.
set -euo pipefail

platform=${1:?usage: package.sh <platform-suffix> <external> [extra-file ...]}
external=${2:?usage: package.sh <platform-suffix> <external> [extra-file ...]}
shift 2

version=${VERSION:-$(git rev-parse --short HEAD)}
# Named after the project, not the external: release assets can't carry the
# ~ that wclap~'s own files need (GitHub rewrites it to a .), and the
# archive's name is packaging, not what Pd looks for inside it.
name="pdwclap-${version}-${platform}"
stage="dist/${name}"

# The external lands in the checkout root (see WCLAP_PD_OUTPUT_DIR), except
# with a multi-config generator, which appends a per-config subdirectory —
# hence the caller passing the path the build actually produced.
if [ ! -f "${external}" ]; then
  echo "package.sh: no external at ${external}" >&2
  exit 1
fi

rm -rf "${stage}"
mkdir -p "${stage}/examples"

cp "${external}" "${stage}/"
cp "wclap~-help.pd" "examples.pd" "LICENSE" "README.md" "${stage}/"
cp -R examples/. "${stage}/examples/"
for extra in "$@"; do
  cp "${extra}" "${stage}/"
done

# Finder droppings travel with a cloned examples/ directory; they have no
# business in the archive.
find "${stage}" -name ".DS_Store" -delete

# cmake's zip writer is the one thing every runner has, and it stores the
# entries relative to the staging directory (no leading ./ or wrapper dir).
(cd "${stage}" && cmake -E tar cf "../${name}.zip" --format=zip .)
echo "dist/${name}.zip"
