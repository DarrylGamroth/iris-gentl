#!/usr/bin/env bash
set -eo pipefail

# The release workflow runs this inside the target distribution container.
# Mount the source at /src read-only and the build workspace at /work.
target=$1
stage=$2
case "$target" in
    debian13|ubuntu24.10)
        if [[ $target == ubuntu24.10 ]]; then
            sed -i -E 's#http://(archive|security)\.ubuntu\.com/ubuntu/#http://old-releases.ubuntu.com/ubuntu/#g' \
                /etc/apt/sources.list.d/ubuntu.sources
        fi
        apt-get update
        if [[ $stage == build ]]; then
            apt-get install -y --no-install-recommends build-essential ca-certificates cmake file libarchive-dev libxml2-dev
        else
            # The base images exclude most documentation by default.
            apt-get install -y --no-install-recommends \
                -o 'Dpkg::Options::=--path-include=/usr/share/doc/iris-gentl/*' \
                gcc libc6-dev /work/release/*.deb
        fi
        libdir=lib/x86_64-linux-gnu
        genapi_version=3.5.1
        ;;
    centos7.9)
        cat >/etc/yum.repos.d/iris-vault.repo <<'REPOS'
[iris-base]
name=CentOS 7.9 base
baseurl=https://archive.kernel.org/centos-vault/7.9.2009/os/$basearch/
gpgcheck=1
gpgkey=file:///etc/pki/rpm-gpg/RPM-GPG-KEY-CentOS-7
[iris-updates]
name=CentOS 7.9 updates
baseurl=https://archive.kernel.org/centos-vault/7.9.2009/updates/$basearch/
gpgcheck=1
gpgkey=file:///etc/pki/rpm-gpg/RPM-GPG-KEY-CentOS-7
[iris-extras]
name=CentOS 7.9 extras
baseurl=https://archive.kernel.org/centos-vault/7.9.2009/extras/$basearch/
gpgcheck=1
gpgkey=file:///etc/pki/rpm-gpg/RPM-GPG-KEY-CentOS-7
[iris-scl]
name=CentOS 7.9 Software Collections
baseurl=https://archive.kernel.org/centos-vault/7.9.2009/sclo/$basearch/rh/
gpgcheck=1
gpgkey=file:///etc/pki/rpm-gpg/RPM-GPG-KEY-CentOS-SIG-SCLo
REPOS
        yum_iris=(yum -y '--disablerepo=*' '--enablerepo=iris-base,iris-updates,iris-extras')
        if [[ $stage == build ]]; then
            "${yum_iris[@]}" install centos-release-scl-rh curl ca-certificates tar gzip libarchive-devel libxml2-devel make
            "${yum_iris[@]}" --enablerepo=iris-scl install devtoolset-11-gcc-c++
            # shellcheck disable=SC1091
            source /opt/rh/devtoolset-11/enable
            cmake_archive=cmake-3.31.6-linux-x86_64.tar.gz
            curl --fail --silent --show-error --location --retry 3 --max-time 120 \
                "https://github.com/Kitware/CMake/releases/download/v3.31.6/$cmake_archive" -o "/tmp/$cmake_archive"
            (cd /tmp && sha256sum -c <<CHECKSUM
5a1133ff103c71eb5120e2cc3de922733e7d8a26a98ae716397e8676adb367bf  $cmake_archive
CHECKSUM
            )
            mkdir -p /opt/iris-cmake
            tar -xzf "/tmp/$cmake_archive" -C /opt/iris-cmake --strip-components=1
            export PATH="/opt/iris-cmake/bin:$PATH"
        else
            "${yum_iris[@]}" install gcc tar gzip libarchive libxml2 libstdc++
        fi
        libdir=lib
        genapi_version=3.5.0
        ;;
    *) echo "Unknown package target: $target" >&2; exit 2 ;;
esac

case "$stage" in
    build)
        cmake -S /src -B /work/build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
            -DIRIS_BUNDLE_GENAPI=ON -DIRIS_GENAPI_VERSION="$genapi_version" \
            -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR="$libdir" -DCMAKE_INSTALL_DOCDIR=share/doc/iris-gentl
        version=$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' /work/build/CMakeCache.txt)
        test -n "$version"
        if [[ -n ${IRIS_RELEASE_TAG:-} && $IRIS_RELEASE_TAG != "v$version" ]]; then
            echo "Release tag $IRIS_RELEASE_TAG does not match CMake version $version" >&2
            exit 1
        fi
        cmake --build /work/build --parallel 4
        ctest --test-dir /work/build --output-on-failure --no-tests=error

        mkdir -p /work/release /work/fixture
        cp /work/build/iris-test-cti.cti /work/fixture/
        if [[ $target == centos7.9 ]]; then
            name="iris-gentl-$version-$target-x86_64"
            cmake --install /work/build --prefix "/work/stage/$name"
            tar -C /work/stage -czf "/work/release/$name.tar.gz" "$name"
        else
            cpack --config /work/build/CPackConfig.cmake -B /work/deb \
                -D "CPACK_PACKAGE_FILE_NAME=iris-gentl_${version}_${target}_amd64"
            cp /work/deb/*.deb /work/release/
        fi
        ;;
    verify)
        prefix=/usr
        if [[ $target == centos7.9 ]]; then
            destination=$(mktemp -d '/tmp/iris relocated.XXXXXX')
            tar -C "$destination" -xzf /work/release/*.tar.gz
            prefixes=("$destination"/iris-gentl-*)
            test "${#prefixes[@]}" -eq 1
            prefix=${prefixes[0]}
        fi
        unset LD_LIBRARY_PATH GENICAM_ROOT
        notices="$prefix/share/doc/iris-gentl/genapi"
        genapi="$prefix/$libdir/iris-gentl/genapi"
        for file in License_ReadMe.txt licenses/GenICam_License.pdf GenApi-SHA256SUMS; do
            if [[ ! -s $notices/$file ]]; then
                echo "Missing GenApi notice or manifest: $notices/$file" >&2
                exit 1
            fi
        done
        (cd "$genapi" && sha256sum -c "$notices/GenApi-SHA256SUMS")
        "$prefix/bin/iris-capture" --list /work/fixture/iris-test-cti.cti
        IRIS_FAULT=progressive-complete "$prefix/bin/iris-gentl" grab \
            --cti /work/fixture/iris-test-cti.cti --stream-only --buffers 1
        gcc -std=c99 -Wall -Wextra -Werror -I"$prefix/include" /src/tests/package_smoke.c \
            -L"$prefix/$libdir" -Wl,-rpath,"$prefix/$libdir" -Wl,-rpath-link,"$genapi" \
            -liris -o /tmp/iris-package-smoke
        IRIS_FAULT=progressive-complete /tmp/iris-package-smoke /work/fixture/iris-test-cti.cti
        ;;
    *) echo "Unknown package stage: $stage" >&2; exit 2 ;;
esac
