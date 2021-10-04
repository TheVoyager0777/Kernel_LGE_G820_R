mkdir -p out
export PATH=~/workspace/android/rom/lineageos/prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9/bin:$PATH
export CROSS_COMPILE=~/workspace/android/rom/lineageos/prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9/bin/aarch64-linux-android-

make O=./out ARCH=arm64 vendor/dragon_alpha_defconfig
make O=./out ARCH=arm64 REAL_CC=~/workspace/android/rom/lineageos/prebuilts/clang/host/linux-x86/clang-r353983c/bin/clang CLANG_TRIPLE=aarch64-linux-gnu- -j16
