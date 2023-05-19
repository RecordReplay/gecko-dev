set -euox pipefail

cd recordreplay
cd $GECKODIR
./mach build
echo "Exit status: $?"
./mach package
echo "Exit status: $?"
MOZ_PRODUCT_VERSION=86.0 MAR_CHANNEL_ID=release MAR=rr-opt/dist/host/bin/mar ./tools/update-packaging/make_full_update.sh rr-opt/replay.complete.mar rr-opt/dist/replay
