#!/bin/sh
# Fetch the two baselines, at the exact commits the comparison reports, into third_party/ (which git ignores: their
# sources are fetched, never kept in this repository). Only their C sources are ever compiled; nothing in them -- no
# script, makefile or test -- is run.
set -e
B="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$B/third_party"
cd "$B/third_party"
[ -d dhara ] || git clone --quiet https://github.com/dlbeer/dhara.git dhara
git -C dhara checkout --quiet 1b166e41b74b4a62ee6001ba5fab7a8805e80ea2          # master, 2022-03-15 (no release tags)
[ -d littlefs ] || git clone --quiet https://github.com/littlefs-project/littlefs.git littlefs
git -C littlefs checkout --quiet v2.11.3                                          # = 6cb4e86540eca0d9ba62500a298385c9d863c8be
git -C dhara log -1 --format='dhara    %H %cd'
git -C littlefs log -1 --format='littlefs %H %cd'
