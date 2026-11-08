#!/bin/sh
# Checks the container image of packaging/docker/Dockerfile from inside it
# (RFC 0038, gate G8): run by .github/workflows/toolchains.yml as
#   docker run --rm -v "$PWD:/work" -w /work <image> sh scripts/check-container.sh
# It installs what the checks need, then runs the toolchain checks on the
# image's /opt/weavec and the corpus gate's quick check of two projects.
set -e
apt-get update -qq
apt-get install -y -qq --no-install-recommends python3 git cmake >/dev/null
# The checkout belongs to the runner's user, not root.
git config --global --add safe.directory "*"
python3 scripts/check-toolchain.py /opt/weavec
python3 scripts/corpus-gate.py --quick --only cJSON --only sds \
  --weavec-cc /opt/weavec/bin/weavec-cc
