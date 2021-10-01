mkdir -p out
export PATH=~/workspace/proton-clang/bin:$PATH
export CROSS_COMPILE=aarch64-linux-gnu-

make O=./out ARCH=arm64 vendor/dragon_alpha_defconfig
make O=./out ARCH=arm64 REAL_CC=clang -j16
