#!/bin/sh -xe

function build_arm
{

NDK=/home/max/Src/android/adk/android-ndk-r8e
PREBUILT=$NDK/toolchains/arm-linux-androideabi-4.4.3/prebuilt/linux-x86_64
PLATFORM=$NDK/platforms/android-8/arch-arm

CPU=armv6
OPTIMIZE_CFLAGS="-marm -march=$CPU"
PREFIX=./android/$CPU 
ADDITIONAL_CONFIGURE_FLAG=

./configure --target-os=linux \
    --prefix=$PREFIX \
    --enable-cross-compile \
    --extra-libs="-lgcc" \
    --arch=arm \
    --cc=$PREBUILT/bin/arm-linux-androideabi-gcc \
    --cross-prefix=$PREBUILT/bin/arm-linux-androideabi- \
    --nm=$PREBUILT/bin/arm-linux-androideabi-nm \
    --sysroot=$PLATFORM \
    --extra-cflags=" -O3 -fpic -DANDROID -DHAVE_SYS_UIO_H=1 -Dipv6mr_interface=ipv6mr_ifindex -fasm -Wno-psabi -fno-short-enums -fno-strict-aliasing -finline-limit=300 $OPTIMIZE_CFLAGS " \
    --disable-shared \
    --enable-static \
    --extra-ldflags="-Wl,-rpath-link=$PLATFORM/usr/lib -L$PLATFORM/usr/lib -nostdlib -lc -lm -ldl -llog" \
    --disable-everything \
    --enable-demuxer=mov \
    --enable-demuxer=h264 \
    --enable-demuxer=asf \
    --disable-ffplay \
    --disable-ffmpeg \
    --enable-protocol=file \
    --enable-avformat \
    --enable-avcodec \
    --enable-decoder=rawvideo \
    --enable-decoder=msmpeg4_crystalhd \
    --enable-decoder=msmpeg4v1 \
    --enable-decoder=msmpeg4v2 \
    --enable-decoder=msmpeg4v3 \
    --enable-decoder=wmalossless \
    --enable-decoder=wmapro \
    --enable-decoder=wmav1 \
    --enable-decoder=wmav2 \
    --enable-decoder=aac \
    --enable-decoder=aac_latm \
    --enable-decoder=mpeg4 \
    --enable-decoder=wmavoice \
    --enable-protocol=mmst \
    --enable-protocol=http \
    --enable-protocol=mmsh \
    --enable-protocol=rtp \
    --enable-demuxer=rtsp \
    --enable-network \
    --enable-zlib \
    --disable-doc 
    
make -j8
make install
$PREBUILT/bin/arm-linux-androideabi-ar d libavcodec/libavcodec.a inverse.o
$PREBUILT/bin/arm-linux-androideabi-ld -rpath-link=$PLATFORM/usr/lib -L$PLATFORM/usr/lib  -soname libffmpeg.so -shared -nostdlib  -z,noexecstack -Bsymbolic --whole-archive --no-undefined -o $PREFIX/libffmpeg.so libavcodec/libavcodec.a libavformat/libavformat.a libavutil/libavutil.a libswscale/libswscale.a -lc -lm -lz -ldl -llog  --warn-once  --dynamic-linker=/system/bin/linker $PREBUILT/lib/gcc/arm-linux-androideabi/4.4.3/libgcc.a
}

function build_x86
{

PREFIX=./android/x86 

rm -rf $PREFIX/*

./configure --target-os=linux \
    --prefix=$PREFIX \
    --sysroot=$PLATFORM \
    --extra-cflags=" -O3 -fpic -fasm -Wno-psabi -fno-short-enums -fno-strict-aliasing -finline-limit=300 $OPTIMIZE_CFLAGS " \
    --enable-shared \
    --disable-static \
    --disable-everything \
    --enable-demuxer=mov \
    --enable-demuxer=h264 \
    --enable-demuxer=asf \
    --disable-ffplay \
    --disable-ffmpeg \
    --enable-protocol=file \
    --enable-avformat \
    --enable-avcodec \
    --enable-decoder=rawvideo \
    --enable-decoder=msmpeg4_crystalhd \
    --enable-decoder=msmpeg4v1 \
    --enable-decoder=msmpeg4v2 \
    --enable-decoder=msmpeg4v3 \
    --enable-decoder=wmalossless \
    --enable-decoder=wmapro \
    --enable-decoder=wmav1 \
    --enable-decoder=wmav2 \
    --enable-decoder=aac \
    --enable-decoder=aac_latm \
    --enable-decoder=mpeg4 \
    --enable-decoder=wmavoice \
    --enable-protocol=mmst \
    --enable-protocol=http \
    --enable-protocol=mmsh \
    --enable-protocol=rtp \
    --enable-demuxer=rtsp \
    --enable-network \
    --enable-zlib \
    --disable-doc 
    
make -j8
make install

#gcc -Landroid/x86/lib -Iandroid/x86/include x86/test.c -o x86/test -lavcodec -lavdevice -lavfilter -lavformat -lavutil -lswresample -lswscale

}

if [ -e config.mak ]; then
    make distclean
fi;

#arm v6
#build_arm
build_x86