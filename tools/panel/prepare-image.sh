#!/bin/sh
# Container paths; creates regular image files only, never opens a block device.
set -eu
export PATH="/out/host/bin:/out/host/sbin:$PATH"
test ! -e /candidate/image
mkdir /candidate/image /candidate/image-qa
unsquashfs -d /tmp/panel-root /baseline/rootfs.squashfs
test "$(sha256sum /tmp/panel-root/usr/bin/tablet-poc | cut -d ' ' -f1)" = 787396e97d2cc5618267ab765035183fa529fa4ff4428ba5b82b37aebf0dd42f
cp /candidate/tablet-panel /tmp/panel-root/usr/bin/tablet-panel
chmod 755 /tmp/panel-root/usr/bin/tablet-panel
# Keep the validated boot-time delay, supervision and RAM logging.
sed 's@/usr/bin/tablet-poc --ui@/usr/bin/tablet-panel --ui@' \
    /tmp/panel-root/usr/bin/run-panel > /tmp/run-panel.new
mv /tmp/run-panel.new /tmp/panel-root/usr/bin/run-panel
chmod 755 /tmp/panel-root/usr/bin/run-panel
mkdir -p /tmp/panel-root/usr/share/licenses/tablet-panel
cp /candidate/LVGL-LICENCE.txt /candidate/cJSON-LICENSE /tmp/panel-root/usr/share/licenses/tablet-panel/
qemu-arm -L /tmp/panel-root /tmp/panel-root/usr/bin/tablet-panel \
    --preview /candidate/image-qa > /candidate/image-self-check.log
mksquashfs /tmp/panel-root /candidate/image/rootfs.squashfs \
    -noappend -comp gzip -all-root -no-xattrs -no-progress
for file in zImage board.dtb boot.scr u-boot-sunxi-with-spl.bin kernel.config u-boot.config; do
    cp "/baseline/$file" "/candidate/image/$file"
done
genimage --rootpath /tmp/panel-root --tmppath /tmp/panel-genimage \
    --inputpath /candidate/image --outputpath /candidate/image \
    --config /project/tools/buildroot/board/genimage.cfg
# Extraction confirms the artifact contains the new supervised app and original recovery app.
unsquashfs -cat /candidate/image/rootfs.squashfs usr/bin/run-panel | grep -q '/usr/bin/tablet-panel --ui'
unsquashfs -cat /candidate/image/rootfs.squashfs usr/bin/tablet-panel > /tmp/panel-installed
cmp /candidate/tablet-panel /tmp/panel-installed
for file in zImage board.dtb boot.scr u-boot-sunxi-with-spl.bin kernel.config u-boot.config; do
    cmp "/baseline/$file" "/candidate/image/$file"
done
echo PANEL_IMAGE_OFFLINE_CHECK_OK
