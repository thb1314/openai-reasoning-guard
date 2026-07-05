#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

TARGET="${TARGET:-linux-x86_64}"
QTBASE_SOURCE_ARCHIVE="${QTBASE_SOURCE_ARCHIVE:-/mnt/data/qt-2080ti-sync/archives/qtbase-everywhere-src-5.15.2.tar.xz}"
OPENSSL_SOURCE_ARCHIVE="${OPENSSL_SOURCE_ARCHIVE:-/mnt/data/qt-2080ti-sync/archives/openssl-1.1.1w.tar.gz}"
OUTPUT_ROOT="${OUTPUT_ROOT:-${PROJECT_DIR}/dist/qt-sdk-build}"
BUILD_DIR="${BUILD_DIR:-}"
PREFIX="${PREFIX:-}"
ARCHIVE="${ARCHIVE:-0}"
UPLOAD="${UPLOAD:-0}"
SET_SECRET="${SET_SECRET:-0}"
UPLOAD_PROXY="${UPLOAD_PROXY:-}"
REPO="${REPO:-thb1314/openai-reasoning-guard}"
RELEASE_TAG="${RELEASE_TAG:-}"
SECRET_NAME="${SECRET_NAME:-}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 2)}"
DOCKER="${DOCKER:-0}"
DOCKER_IMAGE="${DOCKER_IMAGE:-}"
SKIP_DEPS="${SKIP_DEPS:-0}"
CLEAN="${CLEAN:-0}"

usage() {
    cat <<EOF
Usage: $(basename "$0") --target linux-x86_64 [--docker] [--archive] [--upload] [--set-secret]

Build a reusable Qt 5.15.x Linux SDK from qtbase source. GitHub Actions uses
--docker to run non-native targets inside target-architecture Linux containers
through Docker/QEMU; local builds can use the same path when needed.

Targets:
  linux-x86_64   Docker platform linux/amd64, secret QT_LINUX_X86_64_URL
  linux-x86_32   Docker platform linux/386, secret QT_LINUX_X86_32_URL
  linux-arm64    Docker platform linux/arm64, secret QT_LINUX_ARM64_URL
  linux-arm32    Docker platform linux/arm/v7, secret QT_LINUX_ARM32_URL

Examples:
  scripts/build-qt5-linux-sdk.sh --target linux-x86_64 --archive
  scripts/build-qt5-linux-sdk.sh --target linux-arm64 --docker --archive
  scripts/build-qt5-linux-sdk.sh --target linux-x86_64 --archive --upload --set-secret --upload-proxy http://127.0.0.1:7890

Environment overrides:
  QTBASE_SOURCE_ARCHIVE=${QTBASE_SOURCE_ARCHIVE}
  OPENSSL_SOURCE_ARCHIVE=${OPENSSL_SOURCE_ARCHIVE}
  OUTPUT_ROOT=${OUTPUT_ROOT}
  BUILD_DIR=${BUILD_DIR}
  PREFIX=${PREFIX}
  JOBS=${JOBS}
  DOCKER_IMAGE=${DOCKER_IMAGE:-target-specific Debian bookworm image}
EOF
}

while (($# > 0)); do
    case "$1" in
        --target)
            shift
            TARGET="${1:?missing target}"
            ;;
        --qtbase-source-archive)
            shift
            QTBASE_SOURCE_ARCHIVE="${1:?missing qtbase source archive}"
            ;;
        --openssl-source-archive)
            shift
            OPENSSL_SOURCE_ARCHIVE="${1:?missing openssl source archive}"
            ;;
        --output-root)
            shift
            OUTPUT_ROOT="${1:?missing output root}"
            ;;
        --build-dir)
            shift
            BUILD_DIR="${1:?missing build dir}"
            ;;
        --prefix)
            shift
            PREFIX="${1:?missing prefix}"
            ;;
        --jobs)
            shift
            JOBS="${1:?missing jobs}"
            ;;
        --docker)
            DOCKER=1
            ;;
        --docker-image)
            shift
            DOCKER_IMAGE="${1:?missing docker image}"
            ;;
        --skip-deps)
            SKIP_DEPS=1
            ;;
        --clean)
            CLEAN=1
            ;;
        --archive)
            ARCHIVE=1
            ;;
        --upload)
            UPLOAD=1
            ;;
        --set-secret)
            SET_SECRET=1
            ;;
        --upload-proxy)
            shift
            UPLOAD_PROXY="${1:?missing upload proxy}"
            ;;
        --repo)
            shift
            REPO="${1:?missing repo}"
            ;;
        --release-tag)
            shift
            RELEASE_TAG="${1:?missing release tag}"
            ;;
        --secret-name)
            shift
            SECRET_NAME="${1:?missing secret name}"
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
    shift
done

platform_for_target() {
    case "$1" in
        linux-x86_64) echo linux/amd64 ;;
        linux-x86_32) echo linux/386 ;;
        linux-arm64) echo linux/arm64 ;;
        linux-arm32) echo linux/arm/v7 ;;
        *)
            echo "unknown target: $1" >&2
            exit 2
            ;;
    esac
}

docker_image_for_target() {
    case "$1" in
        linux-x86_64) echo debian:bookworm ;;
        linux-x86_32) echo i386/debian:bookworm ;;
        linux-arm64) echo arm64v8/debian:bookworm ;;
        linux-arm32) echo arm32v7/debian:bookworm ;;
        *)
            echo "unknown target: $1" >&2
            exit 2
            ;;
    esac
}

secret_for_target() {
    case "$1" in
        linux-x86_64) echo QT_LINUX_X86_64_URL ;;
        linux-x86_32) echo QT_LINUX_X86_32_URL ;;
        linux-arm64) echo QT_LINUX_ARM64_URL ;;
        linux-arm32) echo QT_LINUX_ARM32_URL ;;
        *)
            echo "unknown target: $1" >&2
            exit 2
            ;;
    esac
}

require_file() {
    local path="$1"
    if [[ ! -f "${path}" ]]; then
        echo "required file missing: ${path}" >&2
        exit 2
    fi
}

restore_output_ownership() {
    if [[ ! -d "${OUTPUT_ROOT}" ]]; then
        return
    fi

    local owner
    owner="$(id -u):$(id -g)"
    if chown -R "${owner}" "${OUTPUT_ROOT}" 2>/dev/null; then
        return
    fi
    if command -v sudo >/dev/null 2>&1; then
        sudo chown -R "${owner}" "${OUTPUT_ROOT}"
        return
    fi

    echo "unable to restore ownership for ${OUTPUT_ROOT}; run chown or use sudo" >&2
    exit 2
}

install_linux_deps() {
    if ((SKIP_DEPS == 1)); then
        return
    fi
    if ! command -v apt-get >/dev/null 2>&1; then
        echo "apt-get not available; install Qt build dependencies manually or pass --skip-deps" >&2
        exit 2
    fi
    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    apt-get install -y --no-install-recommends \
        bash \
        bison \
        build-essential \
        ca-certificates \
        flex \
        gperf \
        libfontconfig1-dev \
        libfreetype6-dev \
        libglib2.0-dev \
        libx11-dev \
        libx11-xcb-dev \
        libxau-dev \
        libxi-dev \
        libxcb1-dev \
        libxcb-glx0-dev \
        libxcb-icccm4-dev \
        libxcb-image0-dev \
        libxcb-keysyms1-dev \
        libxcb-randr0-dev \
        libxcb-render0-dev \
        libxcb-render-util0-dev \
        libxcb-shape0-dev \
        libxcb-shm0-dev \
        libxcb-sync-dev \
        libxcb-util-dev \
        libxcb-xfixes0-dev \
        libxcb-xinerama0-dev \
        libxcb-xkb-dev \
        libxext-dev \
        libxrender-dev \
        libxkbcommon-dev \
        libxkbcommon-x11-dev \
        make \
        patch \
        perl \
        pkg-config \
        python3 \
        ruby \
        tar \
        xz-utils \
        zlib1g-dev
}

run_in_docker() {
    require_file "${QTBASE_SOURCE_ARCHIVE}"
    require_file "${OPENSSL_SOURCE_ARCHIVE}"
    local platform
    platform="$(platform_for_target "${TARGET}")"
    local image="${DOCKER_IMAGE:-$(docker_image_for_target "${TARGET}")}"

    local staged_sources="${PROJECT_DIR}/.package-work/qt-sdk-source-${TARGET}"
    rm -rf "${staged_sources}"
    mkdir -p "${staged_sources}"
    cp -a "${QTBASE_SOURCE_ARCHIVE}" "${staged_sources}/qtbase.tar.xz"
    cp -a "${OPENSSL_SOURCE_ARCHIVE}" "${staged_sources}/openssl.tar.gz"
    mkdir -p "${OUTPUT_ROOT}"

    docker run --rm \
        --platform "${platform}" \
        -v "${PROJECT_DIR}:/workspace" \
        -v "${staged_sources}:/qt-sources:ro" \
        -v "${OUTPUT_ROOT}:/qt-output" \
        -w /workspace \
        "${image}" \
        bash -lc "set -euo pipefail; /workspace/scripts/build-qt5-linux-sdk.sh --target '${TARGET}' --qtbase-source-archive /qt-sources/qtbase.tar.xz --openssl-source-archive /qt-sources/openssl.tar.gz --output-root /qt-output --build-dir /qt-output/build-${TARGET} --prefix /qt-output/qt5-${TARGET} --jobs '${JOBS}' --archive"

    restore_output_ownership

    if ((UPLOAD == 1 || SET_SECRET == 1)); then
        ARCHIVE=1
        PREFIX="${OUTPUT_ROOT}/qt5-${TARGET}"
        BUILD_DIR="${OUTPUT_ROOT}/build-${TARGET}"
        archive_and_maybe_upload
    fi
}

extract_one() {
    local archive="$1"
    local dest="$2"
    rm -rf "${dest}"
    mkdir -p "${dest}"
    tar -xf "${archive}" -C "${dest}" --strip-components=1
}

patch_qtbase_for_modern_cpp() {
    local source_dir="$1"
    local qglobal_header="${source_dir}/src/corelib/global/qglobal.h"
    if [[ -f "${qglobal_header}" ]] && ! grep -q '#include <limits>' "${qglobal_header}"; then
        sed -i '/#include <algorithm>/a #include <limits>' "${qglobal_header}"
    fi

    local qfloat16_header="${source_dir}/src/corelib/global/qfloat16.h"
    if [[ -f "${qfloat16_header}" ]] && ! grep -q '#include <limits>' "${qfloat16_header}"; then
        sed -i '/#include <QtCore\/qglobal.h>/a #include <limits>' "${qfloat16_header}"
    fi

    local qbytearraymatcher_header="${source_dir}/src/corelib/text/qbytearraymatcher.h"
    if [[ -f "${qbytearraymatcher_header}" ]] && ! grep -q '#include <limits>' "${qbytearraymatcher_header}"; then
        sed -i '/#include <QtCore\/qbytearray.h>/a #include <limits>' "${qbytearraymatcher_header}"
    fi

    local configure_script="${source_dir}/configure"
    if [[ -f "${configure_script}" ]] && ! grep -q 'relpathMangled/qtbase.pro' "${configure_script}"; then
        sed -i 's/"\$relpathMangled" --/"$relpathMangled\/qtbase.pro" --/g' "${configure_script}"
    fi
}

qtbase_version() {
    local source_dir="$1"
    sed -n 's/^MODULE_VERSION[[:space:]]*=[[:space:]]*//p' "${source_dir}/.qmake.conf" | head -n 1
}

write_bootstrap_private_module_pri() {
    local source_dir="$1"
    local qt_build="$2"
    local prefix="$3"
    local qt_version
    qt_version="$(qtbase_version "${source_dir}")"
    qt_version="${qt_version:-5.15.2}"
    local major="${qt_version%%.*}"
    local rest="${qt_version#*.}"
    local minor="${rest%%.*}"
    local patch="${qt_version##*.}"

    local qt_prf="${source_dir}/mkspecs/features/qt.prf"
    if [[ -f "${qt_prf}" ]] && ! grep -q 'OpenAI Reasoning Guard bootstrap-private fallback' "${qt_prf}"; then
        local fallback_block
        fallback_block="$(cat <<EOF
# OpenAI Reasoning Guard bootstrap-private fallback for CI cross-architecture Qt builds.
contains(CLEAN_QT, bootstrap_private):isEmpty(QT.bootstrap_private.name) {
    QT.bootstrap_private.VERSION = ${qt_version}
    QT.bootstrap_private.name = QtBootstrap
    QT.bootstrap_private.module = Qt5Bootstrap
    QT.bootstrap_private.libs = ${qt_build}/lib
    QT.bootstrap_private.includes = ${source_dir}/include ${source_dir}/include/QtCore ${source_dir}/include/QtCore/${qt_version} ${source_dir}/include/QtCore/${qt_version}/QtCore ${source_dir}/include/QtXml ${source_dir}/include/QtXml/${qt_version} ${source_dir}/include/QtXml/${qt_version}/QtXml ${qt_build}/include ${qt_build}/include/QtCore ${qt_build}/include/QtXml
    QT.bootstrap_private.frameworks =
    QT.bootstrap_private.depends =
    QT.bootstrap_private.uses =
    QT.bootstrap_private.module_config = v2 staticlib internal_module
    QT.bootstrap_private.CONFIG = gc_binaries
    QT.bootstrap_private.DEFINES = QT_BOOTSTRAP_LIB QT_VERSION_STR=\\'\\\"${qt_version}\\\"\\' QT_VERSION_MAJOR=${major} QT_VERSION_MINOR=${minor} QT_VERSION_PATCH=${patch} QT_BOOTSTRAPPED QT_NO_CAST_TO_ASCII
}
EOF
)"
        local patched_qt_prf="${qt_prf}.patched"
        awk -v block="${fallback_block}" '
            { print }
            $0 == "qt_module_deps = $$CLEAN_QT $$CLEAN_QT_PRIVATE" { print block }
        ' "${qt_prf}" > "${patched_qt_prf}"
        mv "${patched_qt_prf}" "${qt_prf}"
        echo "patched qt.prf bootstrap-private fallback: ${qt_prf}"
    fi

    local modules_inst="${qt_build}/mkspecs/modules-inst"
    mkdir -p "${modules_inst}"
    cat > "${modules_inst}/qt_lib_bootstrap_private.pri" <<EOF
QT.bootstrap_private.VERSION = ${qt_version}
QT.bootstrap_private.name = QtBootstrap
QT.bootstrap_private.module = Qt5Bootstrap
QT.bootstrap_private.libs = \$\$QT_MODULE_HOST_LIB_BASE
QT.bootstrap_private.includes = \$\$QT_MODULE_INCLUDE_BASE \$\$QT_MODULE_INCLUDE_BASE/QtCore \$\$QT_MODULE_INCLUDE_BASE/QtCore/${qt_version} \$\$QT_MODULE_INCLUDE_BASE/QtCore/${qt_version}/QtCore \$\$QT_MODULE_INCLUDE_BASE/QtXml \$\$QT_MODULE_INCLUDE_BASE/QtXml/${qt_version} \$\$QT_MODULE_INCLUDE_BASE/QtXml/${qt_version}/QtXml
QT.bootstrap_private.frameworks =
QT.bootstrap_private.depends =
QT.bootstrap_private.uses =
QT.bootstrap_private.module_config = v2 staticlib internal_module
QT.bootstrap_private.CONFIG = gc_binaries
QT.bootstrap_private.DEFINES = QT_BOOTSTRAP_LIB QT_VERSION_STR=\\'\\"${qt_version}\\"\\' QT_VERSION_MAJOR=${major} QT_VERSION_MINOR=${minor} QT_VERSION_PATCH=${patch} QT_BOOTSTRAPPED QT_NO_CAST_TO_ASCII
QT.bootstrap_private.enabled_features =
QT.bootstrap_private.disabled_features =
QT_CONFIG +=
QT_MODULES += bootstrap
EOF

    local module_dir
    for module_dir in "${source_dir}/mkspecs/modules" "${qt_build}/mkspecs/modules" "${prefix}/mkspecs/modules"; do
        mkdir -p "${module_dir}"
        cat > "${module_dir}/qt_lib_bootstrap_private.pri" <<EOF
QT_MODULE_BIN_BASE = ${qt_build}/bin
QT_MODULE_INCLUDE_BASE = ${source_dir}/include
QT_MODULE_LIB_BASE = ${qt_build}/lib
QT_MODULE_HOST_LIB_BASE = ${qt_build}/lib
include(${modules_inst}/qt_lib_bootstrap_private.pri)
QT.bootstrap_private.priority = 1
EOF
        echo "wrote bootstrap-private module metadata: ${module_dir}/qt_lib_bootstrap_private.pri"
    done
}

patch_qmake_use_pcre2_fallback() {
    local source_dir="$1"
    local qt_build="$2"
    local qmake_use_prf="${source_dir}/mkspecs/features/qmake_use.prf"
    if [[ ! -f "${qmake_use_prf}" ]]; then
        return
    fi
    if grep -q 'OpenAI Reasoning Guard pcre2 fallback' "${qmake_use_prf}"; then
        return
    fi

    local fallback_block
    fallback_block="$(cat <<EOF
# OpenAI Reasoning Guard pcre2 fallback for CI cross-architecture Qt builds.
!defined(QMAKE_LIBS_PCRE2, var) {
    contains(QMAKE_USE_PRIVATE, pcre2)|contains(QMAKE_USE, pcre2) {
        QMAKE_INCDIR_PCRE2 = ${source_dir}/src/3rdparty/pcre2/src
        QMAKE_DEFINES_PCRE2 = PCRE2_CODE_UNIT_WIDTH=16
        QMAKE_LIBS_PCRE2 = ${qt_build}/lib/libqtpcre2.a
    }
}

EOF
)"
    local patched_qmake_use="${qmake_use_prf}.patched"
    {
        printf '%s' "${fallback_block}"
        cat "${qmake_use_prf}"
    } > "${patched_qmake_use}"
    mv "${patched_qmake_use}" "${qmake_use_prf}"
    echo "patched qmake_use.prf pcre2 fallback: ${qmake_use_prf}"
}

patch_qt_config_prefix_build_fallback() {
    local source_dir="$1"
    local qt_build="$2"
    local qt_config_prf="${source_dir}/mkspecs/features/qt_config.prf"
    if [[ ! -f "${qt_config_prf}" ]]; then
        return
    fi
    if grep -q 'OpenAI Reasoning Guard prefix-build qconfig fallback' "${qt_config_prf}"; then
        return
    fi

    local patched_qt_config="${qt_config_prf}.patched"
    awk -v qt_build="${qt_build}" '
        {
            print
            if ($0 == "QMAKE_QT_CONFIG = $$[QT_HOST_DATA/get]/mkspecs/qconfig.pri") {
                print "# OpenAI Reasoning Guard prefix-build qconfig fallback for CI Qt SDK builds."
                print "!exists($$QMAKE_QT_CONFIG):exists(" qt_build "/mkspecs/qconfig.pri): QMAKE_QT_CONFIG = " qt_build "/mkspecs/qconfig.pri"
            }
            if ($0 == "   QMAKE_MODULE_PATH = $$unique(QMAKE_MODULE_PATH)") {
                print "   QMAKE_MODULE_PATH += " qt_build "/mkspecs/modules"
            }
        }
    ' "${qt_config_prf}" > "${patched_qt_config}"
    mv "${patched_qt_config}" "${qt_config_prf}"
    echo "patched qt_config.prf prefix-build fallback: ${qt_config_prf}"
}

sync_prefix_qmake_metadata() {
    local source_dir="$1"
    local qt_build="$2"
    local prefix="$3"
    local prefix_mkspecs="${prefix}/mkspecs"
    mkdir -p "${prefix_mkspecs}"

    # Cross-architecture qmake resolves features from the configured install
    # prefix before `make install` has populated it. Seed the prefix mkspecs
    # tree up front so follow-on submodules can still load `settings.prf` and
    # generated module metadata while qtbase is mid-build.
    if [[ -d "${source_dir}/mkspecs" ]]; then
        cp -a "${source_dir}/mkspecs/." "${prefix_mkspecs}/"
        echo "seeded prefix mkspecs tree: ${prefix_mkspecs}"
    fi
    if [[ -d "${qt_build}/mkspecs/modules" ]]; then
        mkdir -p "${prefix_mkspecs}/modules"
        cp -a "${qt_build}/mkspecs/modules/." "${prefix_mkspecs}/modules/"
        echo "seeded generated qmake modules: ${prefix_mkspecs}/modules"
    fi
    if [[ -d "${qt_build}/mkspecs/modules-inst" ]]; then
        mkdir -p "${prefix_mkspecs}/modules-inst"
        cp -a "${qt_build}/mkspecs/modules-inst/." "${prefix_mkspecs}/modules-inst/"
        echo "seeded generated qmake modules-inst: ${prefix_mkspecs}/modules-inst"
    fi

    local name
    local src
    local dest
    for name in qconfig.pri qmodule.pri; do
        src=""
        for dest in "${qt_build}/mkspecs" "${source_dir}/mkspecs" "${prefix}/mkspecs"; do
            if [[ -f "${dest}/${name}" ]]; then
                src="${dest}/${name}"
                break
            fi
        done
        if [[ -z "${src}" ]]; then
            echo "warning: unable to locate generated ${name} for qmake metadata sync" >&2
            continue
        fi
        for dest in "${qt_build}/mkspecs" "${source_dir}/mkspecs" "${prefix}/mkspecs"; do
            mkdir -p "${dest}"
            if [[ "$(readlink -f "${src}")" == "$(readlink -f "${dest}/${name}" 2>/dev/null || true)" ]]; then
                echo "metadata already in place: ${dest}/${name}"
                continue
            fi
            cp -f "${src}" "${dest}/${name}"
            echo "synced ${name}: ${dest}/${name}"
        done
    done
}

configure_qmake_search_env() {
    local source_dir="$1"
    local qt_build="$2"
    local prefix="$3"
    local path_sep=":"
    local entry
    local -a qmake_paths=()
    local -a qmake_features=()

    for entry in "${qt_build}" "${source_dir}" "${prefix}"; do
        [[ -n "${entry}" ]] && qmake_paths+=("${entry}")
    done
    for entry in \
        "${qt_build}/mkspecs/features" \
        "${source_dir}/mkspecs/features" \
        "${prefix}/mkspecs/features"; do
        [[ -d "${entry}" ]] && qmake_features+=("${entry}")
    done

    if ((${#qmake_paths[@]} > 0)); then
        export QMAKEPATH
        QMAKEPATH="$(IFS="${path_sep}"; echo "${qmake_paths[*]}")${QMAKEPATH:+${path_sep}${QMAKEPATH}}"
        echo "QMAKEPATH=${QMAKEPATH}"
    fi

    if ((${#qmake_features[@]} > 0)); then
        export QMAKEFEATURES
        QMAKEFEATURES="$(IFS="${path_sep}"; echo "${qmake_features[*]}")${QMAKEFEATURES:+${path_sep}${QMAKEFEATURES}}"
        echo "QMAKEFEATURES=${QMAKEFEATURES}"
    fi
}

write_prefix_tool_wrappers() {
    local qt_build="$1"
    local prefix="$2"
    local tool
    mkdir -p "${prefix}/bin"
    for tool in qmake moc rcc tracegen; do
        cat > "${prefix}/bin/${tool}" <<EOF
#!/usr/bin/env bash
# OpenAI Reasoning Guard temporary Qt SDK build wrapper.
exec "${qt_build}/bin/${tool}" "\$@"
EOF
        chmod +x "${prefix}/bin/${tool}"
        echo "wrote temporary Qt tool wrapper: ${prefix}/bin/${tool}"
    done
}

replace_prefix_tool_wrappers() {
    local qt_build="$1"
    local prefix="$2"
    local tool
    for tool in qmake moc rcc tracegen; do
        if [[ -f "${prefix}/bin/${tool}" ]] \
            && grep -q 'OpenAI Reasoning Guard temporary Qt SDK build wrapper' "${prefix}/bin/${tool}" \
            && [[ -x "${qt_build}/bin/${tool}" ]]; then
            cp -f "${qt_build}/bin/${tool}" "${prefix}/bin/${tool}"
            chmod +x "${prefix}/bin/${tool}"
            echo "replaced temporary Qt tool wrapper: ${prefix}/bin/${tool}"
        fi
    done
}

normalize_installed_bootstrap_private_pri() {
    local source_dir="$1"
    local prefix="$2"
    local qt_version
    qt_version="$(qtbase_version "${source_dir}")"
    qt_version="${qt_version:-5.15.2}"
    local major="${qt_version%%.*}"
    local rest="${qt_version#*.}"
    local minor="${rest%%.*}"
    local patch="${qt_version##*.}"

    local module_dir="${prefix}/mkspecs/modules"
    mkdir -p "${module_dir}"
    cat > "${module_dir}/qt_lib_bootstrap_private.pri" <<EOF
QT.bootstrap_private.VERSION = ${qt_version}
QT.bootstrap_private.name = QtBootstrap
QT.bootstrap_private.module = Qt5Bootstrap
QT.bootstrap_private.libs = \$\$QT_MODULE_HOST_LIB_BASE
QT.bootstrap_private.includes = \$\$QT_MODULE_INCLUDE_BASE \$\$QT_MODULE_INCLUDE_BASE/QtCore \$\$QT_MODULE_INCLUDE_BASE/QtCore/${qt_version} \$\$QT_MODULE_INCLUDE_BASE/QtCore/${qt_version}/QtCore \$\$QT_MODULE_INCLUDE_BASE/QtXml \$\$QT_MODULE_INCLUDE_BASE/QtXml/${qt_version} \$\$QT_MODULE_INCLUDE_BASE/QtXml/${qt_version}/QtXml
QT.bootstrap_private.frameworks =
QT.bootstrap_private.depends =
QT.bootstrap_private.uses =
QT.bootstrap_private.module_config = v2 staticlib internal_module
QT.bootstrap_private.CONFIG = gc_binaries
QT.bootstrap_private.DEFINES = QT_BOOTSTRAP_LIB QT_VERSION_STR=\\'\\"${qt_version}\\"\\' QT_VERSION_MAJOR=${major} QT_VERSION_MINOR=${minor} QT_VERSION_PATCH=${patch} QT_BOOTSTRAPPED QT_NO_CAST_TO_ASCII
QT.bootstrap_private.enabled_features =
QT.bootstrap_private.disabled_features =
QT_CONFIG +=
QT_MODULES += bootstrap
EOF
}

build_openssl() {
    local source_dir="${BUILD_DIR}/openssl-src"
    extract_one "${OPENSSL_SOURCE_ARCHIVE}" "${source_dir}"
    (
        cd "${source_dir}"
        if [[ "${TARGET}" == "linux-x86_32" ]]; then
            ./Configure linux-elf no-asm \
                --prefix="${PREFIX}" \
                --openssldir="${PREFIX}/ssl" \
                shared \
                no-ssl3 \
                no-comp
        elif [[ "${TARGET}" == "linux-arm32" ]]; then
            ./Configure linux-generic32 no-asm \
                --prefix="${PREFIX}" \
                --openssldir="${PREFIX}/ssl" \
                shared \
                no-ssl3 \
                no-comp
        else
            ./config --prefix="${PREFIX}" --openssldir="${PREFIX}/ssl" shared no-ssl3 no-comp
        fi
        make -j"${JOBS}"
        make install_sw
    )
}

build_qtbase() {
    local source_dir="${BUILD_DIR}/qtbase-src"
    local qt_build="${BUILD_DIR}/qtbase-build"
    extract_one "${QTBASE_SOURCE_ARCHIVE}" "${source_dir}"
    patch_qtbase_for_modern_cpp "${source_dir}"
    rm -rf "${qt_build}"
    mkdir -p "${qt_build}"
    (
        cd "${qt_build}"
        "${source_dir}/configure" \
            -prefix "${PREFIX}" \
            -opensource \
            -confirm-license \
            -release \
            -shared \
            -no-pch \
            -nomake examples \
            -nomake tests \
            -make libs \
            -make tools \
            -no-dbus \
            -no-gtk \
            -no-cups \
            -no-eglfs \
            -no-linuxfb \
            -no-opengl \
            -no-icu \
            -qt-zlib \
            -qt-pcre \
            -qt-libpng \
            -qt-libjpeg \
            -qt-freetype \
            -qt-harfbuzz \
            -xcb \
            -openssl-runtime \
            -I "${PREFIX}/include" \
            -L "${PREFIX}/lib"
        patch_qt_config_prefix_build_fallback "${source_dir}" "${qt_build}"
        patch_qmake_use_pcre2_fallback "${source_dir}" "${qt_build}"
        write_bootstrap_private_module_pri "${source_dir}" "${qt_build}" "${PREFIX}"
        sync_prefix_qmake_metadata "${source_dir}" "${qt_build}" "${PREFIX}"
        configure_qmake_search_env "${source_dir}" "${qt_build}" "${PREFIX}"
        write_prefix_tool_wrappers "${qt_build}" "${PREFIX}"
        export QMAKEMODULES="${qt_build}/mkspecs/modules${QMAKEMODULES:+:${QMAKEMODULES}}"
        echo "QMAKEMODULES=${QMAKEMODULES}"
        make -j"${JOBS}"
        make install
        replace_prefix_tool_wrappers "${qt_build}" "${PREFIX}"
        normalize_installed_bootstrap_private_pri "${source_dir}" "${PREFIX}"
    )
}

validate_sdk() {
    local required=(
        "${PREFIX}/bin/moc"
        "${PREFIX}/lib/libQt5Core.so.5"
        "${PREFIX}/lib/libQt5Network.so.5"
        "${PREFIX}/lib/libQt5Gui.so.5"
        "${PREFIX}/lib/libQt5Widgets.so.5"
        "${PREFIX}/lib/libQt5Test.so.5"
        "${PREFIX}/plugins/platforms/libqxcb.so"
    )
    local path
    for path in "${required[@]}"; do
        if [[ ! -e "${path}" ]]; then
            echo "built SDK is missing required artifact: ${path}" >&2
            exit 2
        fi
    done
}

archive_and_maybe_upload() {
    validate_sdk
    if ((ARCHIVE == 0 && UPLOAD == 0 && SET_SECRET == 0)); then
        return
    fi

    local archive_dir="${OUTPUT_ROOT}/archives"
    local archive="${archive_dir}/qt5-${TARGET}.tar.xz"
    mkdir -p "${archive_dir}"
    tar -C "$(dirname -- "${PREFIX}")" -cJf "${archive}" "$(basename -- "${PREFIX}")"
    echo "Built Qt SDK archive: ${archive}"
    ls -lh "${archive}"

    if ((UPLOAD == 1 || SET_SECRET == 1)); then
        if ! command -v gh >/dev/null 2>&1; then
            echo "gh CLI is required for --upload or --set-secret" >&2
            exit 2
        fi
        if [[ -n "${UPLOAD_PROXY}" ]]; then
            export HTTP_PROXY="${UPLOAD_PROXY}"
            export HTTPS_PROXY="${UPLOAD_PROXY}"
            export ALL_PROXY="${UPLOAD_PROXY}"
        fi
    fi

    if ((UPLOAD == 1)); then
        if gh release view "${RELEASE_TAG}" -R "${REPO}" >/dev/null 2>&1; then
            gh release upload "${RELEASE_TAG}" "${archive}" -R "${REPO}" --clobber
        else
            gh release create "${RELEASE_TAG}" "${archive}" \
                -R "${REPO}" \
                --title "Qt SDK ${TARGET}" \
                --notes "Qt SDK archive for ${TARGET} GitHub Actions builds." \
                --prerelease
        fi
    fi

    if ((SET_SECRET == 1)); then
        SECRET_NAME="${SECRET_NAME:-$(secret_for_target "${TARGET}")}"
        local asset_url="https://github.com/${REPO}/releases/download/${RELEASE_TAG}/$(basename -- "${archive}")"
        printf '%s' "${asset_url}" | gh secret set "${SECRET_NAME}" -R "${REPO}"
        echo "${SECRET_NAME}=${asset_url}"
    fi
}

if ((DOCKER == 1)); then
    BUILD_DIR="${BUILD_DIR:-${OUTPUT_ROOT}/build-${TARGET}}"
    PREFIX="${PREFIX:-${OUTPUT_ROOT}/qt5-${TARGET}}"
    RELEASE_TAG="${RELEASE_TAG:-qt-sdk-${TARGET}}"
    run_in_docker
    exit 0
fi

require_file "${QTBASE_SOURCE_ARCHIVE}"
require_file "${OPENSSL_SOURCE_ARCHIVE}"
BUILD_DIR="${BUILD_DIR:-${OUTPUT_ROOT}/build-${TARGET}}"
PREFIX="${PREFIX:-${OUTPUT_ROOT}/qt5-${TARGET}}"
RELEASE_TAG="${RELEASE_TAG:-qt-sdk-${TARGET}}"

if ((CLEAN == 1)); then
    rm -rf "${BUILD_DIR}" "${PREFIX}"
fi
mkdir -p "${BUILD_DIR}" "$(dirname -- "${PREFIX}")"

install_linux_deps
build_openssl
build_qtbase
archive_and_maybe_upload

echo "Built Qt SDK: ${PREFIX}"
