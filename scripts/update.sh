#!/usr/bin/env bash
# Copyright (c) 2012 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# This script will check out llvm and clang into third_party/llvm and build it.

# Do NOT CHANGE this if you don't know what you're doing -- see
# https://code.google.com/p/chromium/wiki/UpdatingClang
# Reverting problematic clang rolls is safe, though.
CLANG_REVISION=198389

THIS_DIR="$(dirname "${0}")"
LLVM_DIR="${THIS_DIR}/../../../third_party/llvm"
LLVM_BUILD_DIR="${LLVM_DIR}/../llvm-build"
LLVM_BOOTSTRAP_DIR="${LLVM_DIR}/../llvm-bootstrap"
CLANG_DIR="${LLVM_DIR}/tools/clang"
CLANG_TOOLS_EXTRA_DIR="${CLANG_DIR}/tools/extra"
COMPILER_RT_DIR="${LLVM_DIR}/projects/compiler-rt"
ANDROID_NDK_DIR="${LLVM_DIR}/../android_tools/ndk"
STAMP_FILE="${LLVM_BUILD_DIR}/cr_build_revision"

# ${A:-a} returns $A if it's set, a else.
LLVM_REPO_URL=${LLVM_URL:-https://llvm.org/svn/llvm-project}

if [[ -n "$GYP_DEFINES" ]]; then
  GYP_DEFINES=
fi
if [[ -n "$GYP_GENERATORS" ]]; then
  GYP_GENERATORS=
fi


# Die if any command dies, error on undefined variable expansions.
set -eu

OS="$(uname -s)"

# Parse command line options.
force_local_build=
mac_only=
run_tests=
bootstrap=
with_android=yes
chrome_tools="plugins"

if [[ "${OS}" = "Darwin" ]]; then
  with_android=
fi

while [[ $# > 0 ]]; do
  case $1 in
    --bootstrap)
      bootstrap=yes
      ;;
    --force-local-build)
      force_local_build=yes
      ;;
    --mac-only)
      mac_only=yes
      ;;
    --run-tests)
      run_tests=yes
      ;;
    --without-android)
      with_android=
      ;;
    --with-chrome-tools)
      shift
      if [[ $# == 0 ]]; then
        echo "--with-chrome-tools requires an argument."
        exit 1
      fi
      chrome_tools=$1
      ;;
    --gcc-toolchain)
      shift
      if [[ $# == 0 ]]; then
        echo "--gcc-toolchain requires an argument."
        exit 1
      fi
      if [[ -x "$1/bin/gcc" ]]; then
        gcc_toolchain=$1
      else
        echo "Invalid --gcc-toolchain: '$1'."
        echo "'$1/bin/gcc' does not appear to be valid."
        exit 1
      fi
      ;;

    --help)
      echo "usage: $0 [--force-local-build] [--mac-only] [--run-tests] "
      echo "--bootstrap: First build clang with CC, then with itself."
      echo "--force-local-build: Don't try to download prebuilt binaries."
      echo "--mac-only: Do initial download only on Mac systems."
      echo "--run-tests: Run tests after building. Only for local builds."
      echo "--without-android: Don't build ASan Android runtime library."
      echo "--with-chrome-tools: Select which chrome tools to build." \
           "Defaults to plugins."
      echo "    Example: --with-chrome-tools 'plugins empty-string'"
      echo "--gcc-toolchain: Set the prefix for which GCC version should"
      echo "    be used for building. For example, to use gcc in"
      echo "    /opt/foo/bin/gcc, use '--gcc-toolchain '/opt/foo"
      echo
      exit 1
      ;;
    *)
      echo "Unknown argument: '$1'."
      echo "Use --help for help."
      exit 1
      ;;
  esac
  shift
done

# --mac-only prevents the initial download on non-mac systems, but if clang has
# already been downloaded in the past, this script keeps it up to date even if
# --mac-only is passed in and the system isn't a mac. People who don't like this
# can just delete their third_party/llvm-build directory.
if [[ -n "$mac_only" ]] && [[ "${OS}" != "Darwin" ]] &&
    [[ ! ( "$GYP_DEFINES" =~ .*(clang|tsan|asan|lsan|msan)=1.* ) ]] &&
    ! [[ -d "${LLVM_BUILD_DIR}" ]]; then
  exit 0
fi

# Xcode and clang don't get along when predictive compilation is enabled.
# http://crbug.com/96315
if [[ "${OS}" = "Darwin" ]] && xcodebuild -version | grep -q 'Xcode 3.2' ; then
  XCONF=com.apple.Xcode
  if [[ "${GYP_GENERATORS}" != "make" ]] && \
     [ "$(defaults read "${XCONF}" EnablePredictiveCompilation)" != "0" ]; then
    echo
    echo "          HEARKEN!"
    echo "You're using Xcode3 and you have 'Predictive Compilation' enabled."
    echo "This does not work well with clang (http://crbug.com/96315)."
    echo "Disable it in Preferences->Building (lower right), or run"
    echo "    defaults write ${XCONF} EnablePredictiveCompilation -boolean NO"
    echo "while Xcode is not running."
    echo
  fi

  SUB_VERSION=$(xcodebuild -version | sed -Ene 's/Xcode 3\.2\.([0-9]+)/\1/p')
  if [[ "${SUB_VERSION}" < 6 ]]; then
    echo
    echo "          YOUR LD IS BUGGY!"
    echo "Please upgrade Xcode to at least 3.2.6."
    echo
  fi
fi


# Check if there's anything to be done, exit early if not.
if [[ -f "${STAMP_FILE}" ]]; then
  PREVIOUSLY_BUILT_REVISON=$(cat "${STAMP_FILE}")
  if [[ -z "$force_local_build" ]] && \
       [[ "${PREVIOUSLY_BUILT_REVISON}" = "${CLANG_REVISION}" ]]; then
    echo "Clang already at ${CLANG_REVISION}"
    exit 0
  fi
fi
# To always force a new build if someone interrupts their build half way.
rm -f "${STAMP_FILE}"


# Clobber build files. PCH files only work with the compiler that created them.
# We delete .o files to make sure all files are built with the new compiler.
echo "Clobbering build files"
MAKE_DIR="${THIS_DIR}/../../../out"
XCODEBUILD_DIR="${THIS_DIR}/../../../xcodebuild"
for DIR in "${XCODEBUILD_DIR}" "${MAKE_DIR}/Debug" "${MAKE_DIR}/Release"; do
  if [[ -d "${DIR}" ]]; then
    find "${DIR}" -name '*.o' -exec rm {} +
    find "${DIR}" -name '*.o.d' -exec rm {} +
    find "${DIR}" -name '*.gch' -exec rm {} +
    find "${DIR}" -name '*.dylib' -exec rm -rf {} +
    find "${DIR}" -name 'SharedPrecompiledHeaders' -exec rm -rf {} +
  fi
done

# Clobber NaCl toolchain stamp files, see http://crbug.com/159793
if [[ -d "${MAKE_DIR}" ]]; then
  find "${MAKE_DIR}" -name 'stamp.untar' -exec rm {} +
fi
if [[ "${OS}" = "Darwin" ]]; then
  if [[ -d "${XCODEBUILD_DIR}" ]]; then
    find "${XCODEBUILD_DIR}" -name 'stamp.untar' -exec rm {} +
  fi
fi

if [[ -z "$force_local_build" ]]; then
  # Check if there's a prebuilt binary and if so just fetch that. That's faster,
  # and goma relies on having matching binary hashes on client and server too.
  CDS_URL=https://commondatastorage.googleapis.com/chromium-browser-clang
  CDS_FILE="clang-${CLANG_REVISION}.tgz"
  CDS_OUT_DIR=$(mktemp -d -t clang_download.XXXXXX)
  CDS_OUTPUT="${CDS_OUT_DIR}/${CDS_FILE}"
  if [ "${OS}" = "Linux" ]; then
    CDS_FULL_URL="${CDS_URL}/Linux_x64/${CDS_FILE}"
  elif [ "${OS}" = "Darwin" ]; then
    CDS_FULL_URL="${CDS_URL}/Mac/${CDS_FILE}"
  fi
  echo Trying to download prebuilt clang
  if which curl > /dev/null; then
    curl -L --fail "${CDS_FULL_URL}" -o "${CDS_OUTPUT}" || \
        rm -rf "${CDS_OUT_DIR}"
  elif which wget > /dev/null; then
    wget "${CDS_FULL_URL}" -O "${CDS_OUTPUT}" || rm -rf "${CDS_OUT_DIR}"
  else
    echo "Neither curl nor wget found. Please install one of these."
    exit 1
  fi
  if [ -f "${CDS_OUTPUT}" ]; then
    rm -rf "${LLVM_BUILD_DIR}/Release+Asserts"
    mkdir -p "${LLVM_BUILD_DIR}/Release+Asserts"
    tar -xzf "${CDS_OUTPUT}" -C "${LLVM_BUILD_DIR}/Release+Asserts"
    echo clang "${CLANG_REVISION}" unpacked
    echo "${CLANG_REVISION}" > "${STAMP_FILE}"
    rm -rf "${CDS_OUT_DIR}"
    exit 0
  else
    echo Did not find prebuilt clang at r"${CLANG_REVISION}", building
  fi
fi

if [[ -n "${with_android}" ]] && ! [[ -d "${ANDROID_NDK_DIR}" ]]; then
  echo "Android NDK not found at ${ANDROID_NDK_DIR}"
  echo "The Android NDK is needed to build a Clang whose -fsanitize=address"
  echo "works on Android. See "
  echo "http://code.google.com/p/chromium/wiki/AndroidBuildInstructions for how"
  echo "to install the NDK, or pass --without-android."
  exit 1
fi

echo Getting LLVM r"${CLANG_REVISION}" in "${LLVM_DIR}"
if ! svn co --force "${LLVM_REPO_URL}/llvm/trunk@${CLANG_REVISION}" \
                    "${LLVM_DIR}"; then
  echo Checkout failed, retrying
  rm -rf "${LLVM_DIR}"
  svn co --force "${LLVM_REPO_URL}/llvm/trunk@${CLANG_REVISION}" "${LLVM_DIR}"
fi

echo Getting clang r"${CLANG_REVISION}" in "${CLANG_DIR}"
svn co --force "${LLVM_REPO_URL}/cfe/trunk@${CLANG_REVISION}" "${CLANG_DIR}"

echo Getting compiler-rt r"${CLANG_REVISION}" in "${COMPILER_RT_DIR}"
svn co --force "${LLVM_REPO_URL}/compiler-rt/trunk@${CLANG_REVISION}" \
               "${COMPILER_RT_DIR}"

# Echo all commands.
set -x

NUM_JOBS=3
if [[ "${OS}" = "Linux" ]]; then
  NUM_JOBS="$(grep -c "^processor" /proc/cpuinfo)"
elif [ "${OS}" = "Darwin" ]; then
  NUM_JOBS="$(sysctl -n hw.ncpu)"
fi

if [[ -n "${gcc_toolchain}" ]]; then
  # Use the specified gcc installation for building.
  export CC="$gcc_toolchain/bin/gcc"
  export CXX="$gcc_toolchain/bin/g++"
fi

export CFLAGS=""
export CXXFLAGS=""

# Build bootstrap clang if requested.
if [[ -n "${bootstrap}" ]]; then
  echo "Building bootstrap compiler"
  mkdir -p "${LLVM_BOOTSTRAP_DIR}"
  cd "${LLVM_BOOTSTRAP_DIR}"
  if [[ ! -f ./config.status ]]; then
    # The bootstrap compiler only needs to be able to build the real compiler,
    # so it needs no cross-compiler output support. In general, the host
    # compiler should be as similar to the final compiler as possible, so do
    # keep --disable-threads & co.
    ../llvm/configure \
        --enable-optimized \
        --enable-targets=host-only \
        --disable-threads \
        --disable-pthreads \
        --without-llvmgcc \
        --without-llvmgxx
  fi

  if [[ -n "${gcc_toolchain}" ]]; then
    # Copy that gcc's stdlibc++.so.6 to the build dir, so the bootstrap
    # compiler can start.
    mkdir -p Release+Asserts/lib
    cp -v "$(${CXX} -print-file-name=libstdc++.so.6)" \
      "Release+Asserts/lib/"
  fi


  MACOSX_DEPLOYMENT_TARGET=10.5 make -j"${NUM_JOBS}"
  if [[ -n "${run_tests}" ]]; then
    make check-all
  fi
  cd -
  export CC="${PWD}/${LLVM_BOOTSTRAP_DIR}/Release+Asserts/bin/clang"
  export CXX="${PWD}/${LLVM_BOOTSTRAP_DIR}/Release+Asserts/bin/clang++"

  if [[ -n "${gcc_toolchain}" ]]; then
    # Tell the bootstrap compiler to use a specific gcc prefix to search
    # for standard library headers and shared object file.
    export CFLAGS="--gcc-toolchain=${gcc_toolchain}"
    export CXXFLAGS="--gcc-toolchain=${gcc_toolchain}"
  fi

  echo "Building final compiler"
fi

# Build clang (in a separate directory).
# The clang bots have this path hardcoded in built/scripts/slave/compile.py,
# so if you change it you also need to change these links.
mkdir -p "${LLVM_BUILD_DIR}"
cd "${LLVM_BUILD_DIR}"
if [[ ! -f ./config.status ]]; then
  ../llvm/configure \
      --enable-optimized \
      --disable-threads \
      --disable-pthreads \
      --without-llvmgcc \
      --without-llvmgxx
fi

if [[ -n "${gcc_toolchain}" ]]; then
  # Copy in the right stdlibc++.so.6 so clang can start.
  mkdir -p Release+Asserts/lib
  cp -v "$(${CXX} ${CXXFLAGS} -print-file-name=libstdc++.so.6)" \
    Release+Asserts/lib/
fi
MACOSX_DEPLOYMENT_TARGET=10.5 make -j"${NUM_JOBS}"
STRIP_FLAGS=
if [ "${OS}" = "Darwin" ]; then
  # See http://crbug.com/256342
  STRIP_FLAGS=-x
fi
strip ${STRIP_FLAGS} Release+Asserts/bin/clang
cd -

if [[ -n "${with_android}" ]]; then
  # Make a standalone Android toolchain.
  ${ANDROID_NDK_DIR}/build/tools/make-standalone-toolchain.sh \
      --platform=android-14 \
      --install-dir="${LLVM_BUILD_DIR}/android-toolchain" \
      --system=linux-x86_64 \
      --stl=stlport

  # Build ASan runtime for Android.
  # Note: LLVM_ANDROID_TOOLCHAIN_DIR is not relative to PWD, but to where we
  # build the runtime, i.e. third_party/llvm/projects/compiler-rt.
  cd "${LLVM_BUILD_DIR}"
  make -C tools/clang/runtime/ \
    LLVM_ANDROID_TOOLCHAIN_DIR="../../../llvm-build/android-toolchain"
  cd -
fi

# Build Chrome-specific clang tools. Paths in this list should be relative to
# tools/clang.
# For each tool directory, copy it into the clang tree and use clang's build
# system to compile it.
for CHROME_TOOL_DIR in ${chrome_tools}; do
  TOOL_SRC_DIR="${THIS_DIR}/../${CHROME_TOOL_DIR}"
  TOOL_DST_DIR="${LLVM_DIR}/tools/clang/tools/chrome-${CHROME_TOOL_DIR}"
  TOOL_BUILD_DIR="${LLVM_BUILD_DIR}/tools/clang/tools/chrome-${CHROME_TOOL_DIR}"
  rm -rf "${TOOL_DST_DIR}"
  cp -R "${TOOL_SRC_DIR}" "${TOOL_DST_DIR}"
  rm -rf "${TOOL_BUILD_DIR}"
  mkdir -p "${TOOL_BUILD_DIR}"
  cp "${TOOL_SRC_DIR}/Makefile" "${TOOL_BUILD_DIR}"
  MACOSX_DEPLOYMENT_TARGET=10.5 make -j"${NUM_JOBS}" -C "${TOOL_BUILD_DIR}"
done

if [[ -n "$run_tests" ]]; then
  # Run a few tests.
  for CHROME_TOOL_DIR in ${chrome_tools}; do
    TOOL_SRC_DIR="${THIS_DIR}/../${CHROME_TOOL_DIR}"
    if [[ -f "${TOOL_SRC_DIR}/tests/test.sh" ]]; then
      "${TOOL_SRC_DIR}/tests/test.sh" "${LLVM_BUILD_DIR}/Release+Asserts"
    fi
  done
  cd "${LLVM_BUILD_DIR}"
  make check-all
  cd -
fi

# After everything is done, log success for this revision.
echo "${CLANG_REVISION}" > "${STAMP_FILE}"
