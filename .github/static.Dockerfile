FROM alpine:3.23

RUN apk add --no-cache \
    binutils build-base ca-certificates cmake curl meson ninja pkgconf python3 \
    libx11-dev libx11-static libxext-dev libxext-static libxrender-dev libxinerama-dev \
    libxcb-static libxau-dev libxdmcp-dev fontconfig-dev fontconfig-static \
    freetype-dev freetype-static harfbuzz-dev libpng-static brotli-static expat-static \
    bzip2-static zlib-static \
    font-dejavu tzdata xauth xprop xvfb-run xwininfo

# Alpine ships Xau as a shared library. Build the same release as an archive.
RUN set -eux; \
    cd /tmp; \
    curl -fsSLO --retry 3 https://xorg.freedesktop.org/archive/individual/lib/libXau-1.0.12.tar.xz; \
    echo '74d0e4dfa3d39ad8939e99bda37f5967aba528211076828464d2777d477fc0fb  libXau-1.0.12.tar.xz' | sha256sum -c; \
    tar -xf libXau-1.0.12.tar.xz; \
    cd libXau-1.0.12; \
    ./configure --enable-static --disable-shared; \
    make -j4 install; \
    cd /tmp; \
    rm -rf libXau-1.0.12*

# Enable the FreeType backend CUI uses. HarfBuzz supplies Unicode data itself.
RUN set -eux; \
    cd /tmp; \
    curl -fsSL --retry 3 https://codeload.github.com/harfbuzz/harfbuzz/tar.gz/refs/tags/12.2.0 -o harfbuzz.tar.gz; \
    echo 'f63fc519f150465bd0bdafcdf3d0e9c23474f4c474171cd515ea1b3a72c081fb  harfbuzz.tar.gz' | sha256sum -c; \
    tar -xf harfbuzz.tar.gz; \
    meson setup hb-build harfbuzz-12.2.0 --prefix=/usr/local --libdir=lib --buildtype=release \
        --default-library=static --auto-features=disabled -Dfreetype=enabled \
        -Dtests=disabled -Dutilities=disabled; \
    ninja -C hb-build -j4 src/libharfbuzz.a; \
    mkdir -p /usr/local/include/harfbuzz; \
    cp harfbuzz-12.2.0/src/hb*.h /usr/local/include/harfbuzz/; \
    cp hb-build/src/libharfbuzz.a /usr/local/lib/; \
    cp hb-build/meson-private/harfbuzz.pc /usr/local/lib/pkgconfig/; \
    rm -rf hb-build harfbuzz-12.2.0 harfbuzz.tar.gz

ENV PKG_CONFIG_PATH=/usr/local/lib/pkgconfig

# Use a Fontconfig version that can read current desktop configuration files.
RUN set -eux; \
    apk add --no-cache gperf; \
    cd /tmp; \
    curl -fsSL --retry 3 https://gitlab.freedesktop.org/api/v4/projects/890/packages/generic/fontconfig/2.18.4/fontconfig-2.18.4.tar.xz -o fontconfig.tar.xz; \
    echo '95013637eff5f81d4871dd16cd098c273264c16d0f9f0b27cc43b81b2794bee3  fontconfig.tar.xz' | sha256sum -c; \
    tar -xf fontconfig.tar.xz; \
    cd fontconfig-2.18.4; \
    ./configure --sysconfdir=/etc --localstatedir=/var --enable-static --disable-shared --disable-docs --disable-nls; \
    make -j4; \
    mkdir -p /usr/local/include/fontconfig; \
    cp fontconfig/fontconfig.h fontconfig/fcfreetype.h /usr/local/include/fontconfig/; \
    cp src/.libs/libfontconfig.a /usr/local/lib/; \
    cp fontconfig.pc /usr/local/lib/pkgconfig/; \
    cd /tmp; \
    rm -rf fontconfig-2.18.4 fontconfig.tar.xz
