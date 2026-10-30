#!/bin/sh
# Offline prototype only: never opens USB or writes a boot image.
set -eu
project=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$project"
archive=.tools/downloads/lvgl-v9.4.0.tar.gz
test -f "$archive" || curl -fL --retry 2 \
    https://codeload.github.com/lvgl/lvgl/tar.gz/refs/tags/v9.4.0 -o "$archive"
echo "932c864de98431b38c5758a87f1de12bbee7b76943b8312bcf13d2f29ac14627  $archive" | shasum -a 256 -c -
test -d .tools/sources/lvgl-9.4.0 || tar -xzf "$archive" -C .tools/sources
artifact_rel=${1:-analysis/panel/v5}
mkdir -p .tools/build/panel-arm "$artifact_rel/preview-arm"
docker run --rm -v "$project:/project:ro" \
    -v "$project/.tools/build/panel-arm:/build" \
    -v "$project/$artifact_rel:/artifact" \
    -v tablet-a23-sd-buildroot:/out:ro \
    -w /project/tools/panel tablet-a23-build:sd sh -ec '
        make -s -j6 CC=/out/host/bin/arm-linux-gcc \
            LVGL=/project/.tools/sources/lvgl-9.4.0 OUT=/build \
            LDFLAGS=-Wl,--gc-sections > /artifact/build-arm.log 2>&1
        cp /build/tablet-panel /artifact/tablet-panel
        /out/host/bin/arm-linux-gcc -O2 -Wall -Wextra -Werror -DCJSON_NESTING_LIMIT=8 \
            -I. protocol.c vendor/cJSON.c test-protocol.c -lm -o /build/test-protocol
        qemu-arm -L /out/target /build/test-protocol demo.json > /artifact/test-protocol-arm.log 2>&1
        qemu-arm -L /out/target /artifact/tablet-panel \
            --preview /artifact/preview-arm > /artifact/test-arm.log 2>&1
        file /artifact/tablet-panel
        cat /artifact/test-arm.log
        cat /artifact/test-protocol-arm.log
        cp vendor/cJSON-LICENSE /artifact/cJSON-LICENSE
        cp /project/.tools/sources/lvgl-9.4.0/LICENCE.txt /artifact/LVGL-LICENCE.txt
    '
shasum -a 256 "$artifact_rel/tablet-panel" > "$artifact_rel/SHA256SUMS"
