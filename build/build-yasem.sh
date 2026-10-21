#!/bin/bash
set -euo pipefail
ROOT=/home/builder/yasem-build
SRC=$ROOT/src
DEPS=$ROOT/deps
QTPREFIX=$ROOT/qt
CELIBS=$ROOT/ce-libs
DIST=$ROOT/dist/yasem
LOG=$ROOT/build.log
JOBS=4
export PATH="$QTPREFIX/bin:$PATH"
export CFLAGS="-march=armv7-a -mfpu=neon -mfloat-abi=hard -O2"
export CXXFLAGS="$CFLAGS"
export CC=arm-linux-gnueabihf-gcc
export CXX=arm-linux-gnueabihf-g++
export PKG_CONFIG_PATH="$DEPS/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$DEPS/lib/pkgconfig"

mkdir -p "$SRC" "$DEPS" "$CELIBS" "$DIST"
exec > >(tee -a "$LOG") 2>&1
echo "=== $(date) build start ==="

if [ -z "${YASEM_SSH_PASS:-}" ]; then
    echo "YASEM_SSH_PASS is empty"
    exit 1
fi

sudo apt-get update -qq
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
    gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf \
    build-essential cmake ninja-build ruby ruby-dev gperf bison flex python python3 \
    pkg-config wget xz-utils unzip bzip2 fonts-dejavu-core sshpass >/dev/null

if ! swapon --show | grep -q /swap-yasem; then
    if [ ! -f /swap-yasem ]; then
        sudo fallocate -l 4G /swap-yasem
        sudo chmod 600 /swap-yasem
        sudo mkswap /swap-yasem >/dev/null
    fi
    sudo swapon /swap-yasem || true
fi

echo "=== device smoke test ==="
echo 'int main(void){return 7;}' | arm-linux-gnueabihf-gcc -march=armv7-a -mfpu=neon -mfloat-abi=hard -x c -o "$ROOT/yasem-hello" -
sshpass -p "$YASEM_SSH_PASS" scp -o StrictHostKeyChecking=no "$ROOT/yasem-hello" ${YASEM_SSH_HOST:-root@YOUR_BOX_IP}:/tmp/yasem-hello
set +e
sshpass -p "$YASEM_SSH_PASS" ssh -o StrictHostKeyChecking=no ${YASEM_SSH_HOST:-root@YOUR_BOX_IP} /tmp/yasem-hello
hello_code=$?
set -e
echo "hello exit $hello_code"
if [ "$hello_code" != "7" ]; then
    echo "ARM binary did not run on CoreELEC"
    exit 1
fi

echo "=== CoreELEC libraries ==="
fetch_lib() {
    local remote="$1"
    local name="$2"
    if [ ! -s "$CELIBS/$name" ]; then
        sshpass -p "$YASEM_SSH_PASS" scp -o StrictHostKeyChecking=no "${YASEM_SSH_HOST:-root@YOUR_BOX_IP}:$remote" "$CELIBS/$name"
    fi
}
fetch_lib /usr/lib/libavcodec.so.58 libavcodec.so.58
fetch_lib /usr/lib/libavformat.so.58 libavformat.so.58
fetch_lib /usr/lib/libavutil.so.56 libavutil.so.56
fetch_lib /usr/lib/libswscale.so.5 libswscale.so.5
fetch_lib /usr/lib/libswresample.so.3 libswresample.so.3
fetch_lib /usr/lib/libamcodec.so.0.0 libamcodec.so
fetch_lib /usr/lib/libasound.so.2 libasound.so.2
fetch_lib /lib/libssl.so.1.1 libssl.so.1.1
fetch_lib /lib/libcrypto.so.1.1 libcrypto.so.1.1
fetch_lib /lib/libz.so.1 libz.so.1
cd "$CELIBS"
ln -sfn libavcodec.so.58 libavcodec.so
ln -sfn libavformat.so.58 libavformat.so
ln -sfn libavutil.so.56 libavutil.so
ln -sfn libswscale.so.5 libswscale.so
ln -sfn libswresample.so.3 libswresample.so
ln -sfn libasound.so.2 libasound.so
ln -sfn libssl.so.1.1 libssl.so
ln -sfn libcrypto.so.1.1 libcrypto.so
ln -sfn libz.so.1 libz.so

download() {
    local url="$1"
    local out="$2"
    if [ -s "$out" ]; then
        return 0
    fi
    echo "download $url"
    wget -O "$out.part" "$url" || return 1
    mv "$out.part" "$out"
}

download https://github.com/unicode-org/icu/releases/download/release-65-1/icu4c-65_1-src.tgz "$SRC/icu4c-65_1-src.tgz"
download https://zlib.net/zlib-1.2.11.tar.gz "$SRC/zlib-1.2.11.tar.gz" || download https://github.com/madler/zlib/archive/refs/tags/v1.2.11.tar.gz "$SRC/zlib-1.2.11.tar.gz"
download https://download.gnome.org/sources/libxml2/2.9/libxml2-2.9.10.tar.xz "$SRC/libxml2-2.9.10.tar.xz"
download https://download.gnome.org/sources/libxslt/1.1/libxslt-1.1.34.tar.xz "$SRC/libxslt-1.1.34.tar.xz"
download https://www.sqlite.org/2021/sqlite-autoconf-3340100.tar.gz "$SRC/sqlite-autoconf-3340100.tar.gz"
download https://ffmpeg.org/releases/ffmpeg-4.3.2.tar.xz "$SRC/ffmpeg-4.3.2.tar.xz"
download https://www.openssl.org/source/old/1.1.1/openssl-1.1.1w.tar.gz "$SRC/openssl-1.1.1w.tar.gz"
download https://www.alsa-project.org/files/pub/lib/alsa-lib-1.2.4.tar.bz2 "$SRC/alsa-lib-1.2.4.tar.bz2"
download https://ftp.fau.de/qtproject/archive/qt/5.15/5.15.2/submodules/qtbase-everywhere-src-5.15.2.tar.xz "$SRC/qtbase-official.tar.xz"
download https://github.com/qtwebkit/qtwebkit/releases/download/qtwebkit-5.212.0-alpha4/qtwebkit-5.212.0-alpha4.tar.xz "$SRC/qtwebkit.tar.xz"
download https://www.ijg.org/files/jpegsrc.v9d.tar.gz "$SRC/jpegsrc.v9d.tar.gz" \
    || download https://ftp.osuosl.org/pub/blfs/conglomeration/libjpeg/jpegsrc.v9d.tar.gz "$SRC/jpegsrc.v9d.tar.gz"
download https://download.sourceforge.net/libpng/libpng-1.6.37.tar.xz "$SRC/libpng-1.6.37.tar.xz" \
    || download https://github.com/pnggroup/libpng/releases/download/v1.6.37/libpng-1.6.37.tar.xz "$SRC/libpng-1.6.37.tar.xz"

if [ ! -d "$SRC/icu" ]; then tar -xzf "$SRC/icu4c-65_1-src.tgz" -C "$SRC"; fi
if [ ! -d "$SRC/zlib-1.2.11" ] && [ ! -d "$SRC/zlib-1.2.11-src" ]; then
    tar -xzf "$SRC/zlib-1.2.11.tar.gz" -C "$SRC" || true
fi
if [ ! -d "$SRC/libxml2-2.9.10" ]; then tar -xJf "$SRC/libxml2-2.9.10.tar.xz" -C "$SRC"; fi
if [ ! -d "$SRC/libxslt-1.1.34" ]; then tar -xJf "$SRC/libxslt-1.1.34.tar.xz" -C "$SRC"; fi
if [ ! -d "$SRC/sqlite-autoconf-3340100" ]; then tar -xzf "$SRC/sqlite-autoconf-3340100.tar.gz" -C "$SRC"; fi
if [ ! -d "$SRC/ffmpeg-4.3.2" ]; then tar -xJf "$SRC/ffmpeg-4.3.2.tar.xz" -C "$SRC"; fi
if [ ! -d "$SRC/openssl-1.1.1w" ]; then tar -xzf "$SRC/openssl-1.1.1w.tar.gz" -C "$SRC"; fi
if [ ! -f "$SRC/openssl-1.1.1w/include/openssl/opensslconf.h" ]; then
    echo "=== openssl headers ==="
    (
        cd "$SRC/openssl-1.1.1w"
        ./Configure linux-armv4 no-shared \
            --prefix="$DEPS" --cross-compile-prefix=arm-linux-gnueabihf-
        make include/openssl/opensslconf.h
    )
fi
if [ ! -d "$SRC/alsa-lib-1.2.4" ]; then tar -xjf "$SRC/alsa-lib-1.2.4.tar.bz2" -C "$SRC"; fi
if [ ! -d "$SRC/qtbase-everywhere-src-5.15.2" ]; then tar -xJf "$SRC/qtbase-official.tar.xz" -C "$SRC"; fi
if [ ! -d "$SRC/qtwebkit-5.212.0-alpha4" ]; then tar -xJf "$SRC/qtwebkit.tar.xz" -C "$SRC"; fi

if [ ! -f "$DEPS/.zlib-done" ]; then
    echo "=== zlib ==="
    ZLIB_SRC="$SRC/zlib-1.2.11"
    if [ ! -d "$ZLIB_SRC" ]; then ZLIB_SRC="$SRC/zlib-1.2.11-src"; fi
    rm -rf "$ROOT/zlib-build"
    mkdir -p "$ROOT/zlib-build"
    tar -xzf "$SRC/zlib-1.2.11.tar.gz" -C "$ROOT/zlib-build" --strip-components=0 || true
    cd "$ZLIB_SRC"
    make distclean >/dev/null 2>&1 || true
    CC=$CC CFLAGS="$CFLAGS" ./configure --prefix="$DEPS"
    make -j"$JOBS"
    make install
    touch "$DEPS/.zlib-done"
fi

if [ ! -f "$DEPS/.icu-done" ]; then
    echo "=== icu ==="
    cd "$SRC/icu/source"
    rm -rf build-native build-arm
    mkdir build-native
    (cd build-native && CC=gcc CXX=g++ CFLAGS="-O2" CXXFLAGS="-O2" ../configure --prefix="$ROOT/icu-native" && CC=gcc CXX=g++ CFLAGS="-O2" CXXFLAGS="-O2" make -j"$JOBS")
    mkdir build-arm
    (cd build-arm && CC=arm-linux-gnueabihf-gcc CXX=arm-linux-gnueabihf-g++ CFLAGS="$CFLAGS" CXXFLAGS="$CXXFLAGS" \
        ../configure --host=arm-linux-gnueabihf --prefix="$DEPS" \
        --with-cross-build="$SRC/icu/source/build-native" \
        --disable-samples --disable-tests --disable-extras && make -j"$JOBS" && make install)
    python3 /path/to/yasem-project/build/patch-icudata.py "$DEPS/lib/libicudata.so.65.1"
    touch "$DEPS/.icu-done"
fi
if readelf -h "$DEPS/lib/libicudata.so.65.1" | grep -q "soft-float"; then
    python3 /path/to/yasem-project/build/patch-icudata.py "$DEPS/lib/libicudata.so.65.1"
fi

if [ ! -f "$DEPS/.xml-done" ]; then
    echo "=== libxml2 libxslt sqlite ==="
    cd "$SRC/libxml2-2.9.10"
    make distclean >/dev/null 2>&1 || true
    ./configure --host=arm-linux-gnueabihf --prefix="$DEPS" --without-python --without-lzma --with-zlib="$DEPS"
    make -j"$JOBS"
    make install
    cd "$SRC/libxslt-1.1.34"
    make distclean >/dev/null 2>&1 || true
    ./configure --host=arm-linux-gnueabihf --prefix="$DEPS" --without-python --with-libxml-prefix="$DEPS"
    make -j"$JOBS"
    make install
    cd "$SRC/sqlite-autoconf-3340100"
    make distclean >/dev/null 2>&1 || true
    ./configure --host=arm-linux-gnueabihf --prefix="$DEPS" --disable-static --enable-shared
    make -j"$JOBS"
    make install
    touch "$DEPS/.xml-done"
fi

if [ ! -f "$QTPREFIX/.qtbase-done" ]; then
    echo "=== qtbase ==="
    QTBASE="$SRC/qtbase-everywhere-src-5.15.2"
    SPEC="$QTBASE/mkspecs/linux-arm-gnueabihf-g++"
    rm -rf "$SPEC"
    cp -a "$QTBASE/mkspecs/linux-arm-gnueabi-g++" "$SPEC"
    sed -i 's/arm-linux-gnueabi-/arm-linux-gnueabihf-/g' "$SPEC/qmake.conf"
    printf '\nQMAKE_CFLAGS += -march=armv7-a -mfpu=neon -mfloat-abi=hard\nQMAKE_CXXFLAGS += -march=armv7-a -mfpu=neon -mfloat-abi=hard\n' >> "$SPEC/qmake.conf"
    rm -rf "$ROOT/qtbase-build"
    mkdir -p "$ROOT/qtbase-build"
    cd "$ROOT/qtbase-build"
    unset CC CXX
    export CPATH="$SRC/openssl-1.1.1w/include"
    export LIBRARY_PATH="$CELIBS"
    "$QTBASE/configure" -opensource -confirm-license \
        -release -optimize-size \
        -prefix "$QTPREFIX" \
        -xplatform linux-arm-gnueabihf-g++ \
        -device-option CROSS_COMPILE=arm-linux-gnueabihf- \
        -nomake examples -nomake tests -no-compile-examples \
        -no-xcb -no-opengl \
        -no-cups -no-dbus -no-glib -no-icu -no-fontconfig \
        -qt-zlib -qt-libpng -qt-libjpeg -qt-pcre -qt-freetype -qt-harfbuzz \
        -openssl-runtime \
        -no-pch \
        -evdev \
        -no-libinput \
        -v
    make -j"$JOBS"
    make install
    touch "$QTPREFIX/.qtbase-done"
fi

if [ ! -f "$DEPS/.image-done" ]; then
    echo "=== libjpeg libpng ==="
    if [ ! -d "$SRC/jpeg-9d" ]; then tar -xzf "$SRC/jpegsrc.v9d.tar.gz" -C "$SRC"; fi
    if [ ! -d "$SRC/libpng-1.6.37" ]; then tar -xJf "$SRC/libpng-1.6.37.tar.xz" -C "$SRC"; fi
    (
        cd "$SRC/jpeg-9d"
        make distclean >/dev/null 2>&1 || true
        ./configure --host=arm-linux-gnueabihf --prefix="$DEPS" --disable-static --enable-shared
        make -j"$JOBS"
        make install
    )
    (
        cd "$SRC/libpng-1.6.37"
        make distclean >/dev/null 2>&1 || true
        ./configure --host=arm-linux-gnueabihf --prefix="$DEPS" --disable-static --enable-shared \
            CPPFLAGS="-I$DEPS/include" LDFLAGS="-L$DEPS/lib"
        make -j"$JOBS"
        make install
    )
    touch "$DEPS/.image-done"
fi

if [ ! -f "$QTPREFIX/.qtwebkit-done" ]; then
    echo "=== qtwebkit ==="
    cat > "$ROOT/toolchain.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)
set(CMAKE_FIND_ROOT_PATH ${DEPS} ${QTPREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF
    rm -rf "$ROOT/qtwebkit-build"
    mkdir -p "$ROOT/qtwebkit-build"
    cd "$ROOT/qtwebkit-build"
    cmake -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DPORT=Qt \
        -DCMAKE_PREFIX_PATH="$QTPREFIX;$DEPS" \
        -DCMAKE_TOOLCHAIN_FILE="$ROOT/toolchain.cmake" \
        -DENABLE_API_TESTS=OFF \
        -DENABLE_TOOLS=OFF \
        -DENABLE_WEBKIT2=OFF \
        -DENABLE_PRINT_SUPPORT=OFF \
        -DENABLE_QT_WEBCHANNEL=OFF \
        -DUSE_GSTREAMER=OFF \
        -DUSE_LIBHYPHEN=OFF \
        -DENABLE_VIDEO=OFF \
        -DENABLE_WEB_AUDIO=OFF \
        -DENABLE_OPENGL=OFF \
        -DENABLE_GEOLOCATION=OFF \
        -DENABLE_DEVICE_ORIENTATION=OFF \
        -DENABLE_GAMEPAD_DEPRECATED=OFF \
        -DUSE_LD_GOLD=OFF \
        -DSKIP_DWZ=ON \
        -DCMAKE_C_FLAGS="$CFLAGS" \
        -DCMAKE_CXX_FLAGS="$CXXFLAGS -std=gnu++14" \
        -DCMAKE_INSTALL_PREFIX="$QTPREFIX" \
        "$SRC/qtwebkit-5.212.0-alpha4"
    ninja -j2
    ninja install
    touch "$QTPREFIX/.qtwebkit-done"
fi

echo "=== yasem ==="
export AMCODEC=/path/to/yasem-project/build/amcodec/amcodec
export FFMPEG=$SRC/ffmpeg-4.3.2
export ALSA=$SRC/alsa-lib-1.2.4
export OPENSSL=$SRC/openssl-1.1.1w
export CELIBS
rm -rf "$ROOT/yasem-obj"
mkdir -p "$ROOT/yasem-obj"
cd "$ROOT/yasem-obj"
"$QTPREFIX/bin/qmake" /path/to/yasem-project/yasem/yasem.pro
make -j"$JOBS"

echo "=== bundle ==="
rm -rf "$DIST"
mkdir -p "$DIST/lib" "$DIST/plugins" "$DIST/fonts" "$DIST/config"
cp "$ROOT/yasem-obj/yasem" "$DIST/yasem"
cp /path/to/yasem-project/yasem/start.sh "$DIST/start.sh"
cp /path/to/yasem-project/yasem/run-instead-of-kodi.sh "$DIST/run-instead-of-kodi.sh"
sed -i 's/\r$//' "$DIST/start.sh" "$DIST/run-instead-of-kodi.sh"
chmod 755 "$DIST/yasem" "$DIST/start.sh" "$DIST/run-instead-of-kodi.sh"
cp /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf "$DIST/fonts/"
cp /usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf "$DIST/fonts/" || true
cat > "$DIST/qt.conf" <<'EOF'
[Paths]
Prefix=.
Plugins=plugins
Libraries=lib
EOF

copy_deps() {
    local bin="$1"
    arm-linux-gnueabihf-readelf -d "$bin" | awk '/NEEDED/ {print $5}' | tr -d '[]' | while read -r soname; do
        case "$soname" in
            libc.so*|libm.so*|libdl.so*|libpthread.so*|librt.so*|libstdc++.so*|libgcc_s.so*|ld-linux*|libamcodec.so*|libav*.so*|libsw*.so*|libasound.so*|libcrypto.so*|libssl.so*|libz.so*)
                continue
                ;;
        esac
        if [ -e "$DIST/lib/$soname" ]; then
            continue
        fi
        found=""
        for dir in "$QTPREFIX/lib" "$DEPS/lib" "$CELIBS"; do
            if [ -e "$dir/$soname" ]; then
                found="$dir/$soname"
                break
            fi
        done
        if [ -z "$found" ]; then
            echo "missing $soname for $bin"
            continue
        fi
        cp -a "$found" "$DIST/lib/$soname"
        if [ -L "$found" ]; then
            target=$(readlink -f "$found")
            cp -a "$target" "$DIST/lib/$(basename "$target")"
        fi
        copy_deps "$DIST/lib/$soname"
    done
}
copy_deps "$DIST/yasem"

mkdir -p "$DIST/plugins/platforms" "$DIST/plugins/generic" "$DIST/plugins/imageformats" "$DIST/plugins/bearer"
cp -a "$QTPREFIX/plugins/platforms/libqlinuxfb.so" "$DIST/plugins/platforms/" || true
cp -a "$QTPREFIX/plugins/generic/"*.so "$DIST/plugins/generic/" || true
cp -a "$QTPREFIX/plugins/imageformats/"*.so "$DIST/plugins/imageformats/" || true
cp -a "$QTPREFIX/plugins/bearer/"*.so "$DIST/plugins/bearer/" || true
for plugin in "$DIST/plugins/platforms/"*.so "$DIST/plugins/generic/"*.so "$DIST/plugins/imageformats/"*.so "$DIST/plugins/bearer/"*.so; do
    [ -f "$plugin" ] || continue
    copy_deps "$plugin"
done

echo "=== deploy ==="
sshpass -p "$YASEM_SSH_PASS" ssh -o StrictHostKeyChecking=no ${YASEM_SSH_HOST:-root@YOUR_BOX_IP} "mkdir -p /storage/yasem"
tar -C "$DIST" -cf - . | sshpass -p "$YASEM_SSH_PASS" ssh -o StrictHostKeyChecking=no ${YASEM_SSH_HOST:-root@YOUR_BOX_IP} "tar -C /storage/yasem -xf -"
sshpass -p "$YASEM_SSH_PASS" ssh -o StrictHostKeyChecking=no ${YASEM_SSH_HOST:-root@YOUR_BOX_IP} "chmod 755 /storage/yasem/yasem /storage/yasem/start.sh /storage/yasem/run-instead-of-kodi.sh && ls -lh /storage/yasem/yasem && echo DEPLOYED"
echo "=== $(date) build finished ==="
