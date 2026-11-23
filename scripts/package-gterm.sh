#!/usr/bin/env bash
set -euo pipefail
root=$(git rev-parse --show-toplevel)
work=${1:?pass an absolute build work directory}
output=${2:?pass an absolute asset output directory}
key=${3:?pass the raw distribution public key}
mkdir -p "$work" "$output"
export TMPDIR="$work" TMP="$work" TEMP="$work"
cd "$root"

options=()
crypto_license=
case "$(uname -s):$(uname -m)" in
    Darwin:arm64)
        platform=macos-arm64
        crypto_archive="$work/openssl-3.5.4.tar.gz"
        curl --fail --silent --show-error --location --proto '=https' --proto-redir '=https' \
            https://www.openssl.org/source/openssl-3.5.4.tar.gz --output "$crypto_archive"
        printf '%s  %s\n' 967311f84955316969bdb1d8d4b983718ef42338639c621ec4c34fddef355e99 \
            "$crypto_archive" | shasum -a 256 --check
        tar -xzf "$crypto_archive" -C "$work"
        (
            cd "$work/openssl-3.5.4"
            export MACOSX_DEPLOYMENT_TARGET=13.0
            ./Configure darwin64-arm64-cc no-shared no-module no-tests --prefix="$work/openssl"
            make -j "$(sysctl -n hw.ncpu)" build_libs
            make install_dev
        )
        crypto_license="$work/openssl-3.5.4/LICENSE.txt"
        options+=("-DOPENSSL_ROOT_DIR=$work/openssl" -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0)
        ;;
    Linux:x86_64) platform=linux-x86_64 ;;
    Linux:aarch64) platform=linux-arm64 ;;
    *) printf 'Unsupported platform\n' >&2; exit 1 ;;
esac

if [[ $platform == linux-* ]]; then
    # Link ICU statically so the Executor works without a distro-specific ICU SONAME.
    icu_path=$(pkg-config --variable=libdir icu-uc)
    options+=(-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
              "-DICU_UC_LIBRARY_RELEASE=$icu_path/libicuuc.a"
              "-DICU_DATA_LIBRARY_RELEASE=$icu_path/libicudata.a"
              '-DCMAKE_EXE_LINKER_FLAGS=-static-libstdc++'
              '-DGTERM_STATIC_ICU=ON')
fi
archive="$work/lua-5.5.1.tar.gz"
curl --fail --silent --show-error --location --proto '=https' --proto-redir '=https' \
    https://www.lua.org/ftp/lua-5.5.1.tar.gz --output "$archive"
cmake -S cli/gterm -B "$work/build" -DCMAKE_BUILD_TYPE=Release \
    -DGHOSTERM_WITH_LIBSSH2=OFF -DGHOSTERM_WITH_PLUGIN_LUA=ON \
    -DGHOSTERM_WITH_RELEASE_SIGNATURE=ON -DOPENSSL_USE_STATIC_LIBS=TRUE \
    "-DGHOSTERM_LUA_ARCHIVE=$archive" "-DGTERM_UPDATE_PUBLIC_KEY=$key" "${options[@]}"
cmake --build "$work/build" --parallel
cmake --install "$work/build" --prefix "$work/payload"

licenses="$work/payload/share/licenses/gterm"
if [[ $platform == linux-* ]]; then
    cp /usr/share/doc/libicu-dev/copyright "$licenses/ICU.txt"
    cp /usr/share/doc/libssl-dev/copyright "$licenses/OpenSSL.txt"
    cp /usr/share/doc/libstdc++6/copyright "$licenses/libstdc++.txt"
else
    cp "$crypto_license" "$licenses/OpenSSL.txt"
fi
"$work/payload/bin/gterm" doctor
if [[ $platform == linux-* ]]; then
    dependencies=$(ldd "$work/payload/bin/gterm")
    if grep -E 'lib(icu|crypto|stdc\+\+)|not found' <<<"$dependencies"; then
        printf 'Unexpected runtime dependencies\n' >&2
        exit 1
    fi
else
    otool -L "$work/payload/bin/gterm"
fi
tar -C "$work/payload" -czf "$output/gterm-$platform.tar.gz" bin share
