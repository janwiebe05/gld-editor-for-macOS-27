#!/bin/zsh
# gld-patch.sh – makes Allen & Heath GLD Editor 1.61 run on macOS 27 (Apple Silicon/Intel)
#
# Builds a few small compatibility libraries from source and applies them to a
# COPY of your own GLD Editor 1.61 installation. The original is not modified.
#
# Usage: ./gld-patch.sh [--source <app>] [--dest <app>] [--force] [--test]
#
#   --source  original GLD Editor 1.61.app      (default: /Applications/GLD Editor 1.61.app)
#   --dest    where to put the patched copy     (default: /Applications/GLD Editor 1.61 (macOS 27).app,
#                                                or ~/Applications if /Applications is not writable)
#   --force   patch even if the source is not the known GLD Editor 1.61 build
#   --test    start the patched app once and check that it keeps running
#
# Unofficial, not affiliated with or endorsed by Allen & Heath.
set -euo pipefail

HERE=${0:A:h}
BUILD="$HERE/build"
OUT="$BUILD/out"
SRC="/Applications/GLD Editor 1.61.app"
DST=""
FORCE=0
TEST=0

# Known-good GLD Editor 1.61 (Rev. 38134) main executable
GLD_SHA256=598eb31a3f2dc3f46ef291d1974633f23b685d46aaf58bae7daec58b8f108334

BZIP2_URL=https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz
BZIP2_SHA256=ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269
LIBXML2_URL=https://download.gnome.org/sources/libxml2/2.9/libxml2-2.9.14.tar.xz
LIBXML2_SHA256=60d74a257d1ccec0475e749cba2f21559e48139efba6ff28224357c7c798dfee

CFLAGS=(-arch x86_64 -mmacosx-version-min=10.13 -O2)

while (( $# )); do
  case "$1" in
    --source) SRC="$2"; shift 2 ;;
    --dest) DST="$2"; shift 2 ;;
    --force) FORCE=1; shift ;;
    --test) TEST=1; shift ;;
    -h|--help) sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "Unknown option: $1 (see --help)"; exit 1 ;;
  esac
done

if [[ -z "$DST" ]]; then
  if [[ -w /Applications ]]; then
    DST="/Applications/GLD Editor 1.61 (macOS 27).app"
  else
    DST="$HOME/Applications/GLD Editor 1.61 (macOS 27).app"
  fi
fi

step() { print -P "%B==> $1%b"; }
fail() { print -P "%F{red}Error:%f $1" >&2; exit 1; }

# ---------------------------------------------------------------- checks

step "Checking requirements"

[[ $(uname -s) == Darwin ]] || fail "this script runs on macOS only"
for tool in clang install_name_tool otool python3 curl shasum; do
  command -v $tool >/dev/null || fail "'$tool' not found – install the Command Line Tools: xcode-select --install"
done
# python3 from the Command Line Tools is a stub until they are installed
python3 -c "" 2>/dev/null || fail "python3 is not usable – install the Command Line Tools: xcode-select --install"

if [[ $(uname -m) == arm64 ]] && ! arch -x86_64 /usr/bin/true 2>/dev/null; then
  print "Rosetta 2 is not installed. GLD Editor is an Intel app and needs it."
  read -q "?Install Rosetta now? [y/N] " || { print; fail "Rosetta is required"; }
  print
  softwareupdate --install-rosetta --agree-to-license
fi

[[ -d "$SRC" ]] || fail "GLD Editor not found at: $SRC (use --source)"
version=$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" "$SRC/Contents/Info.plist" 2>/dev/null || true)
sum=$(shasum -a 256 "$SRC/Contents/MacOS/GLD Editor" | awk '{print $1}')
if [[ "$sum" != "$GLD_SHA256" ]]; then
  if [[ -f "$SRC/Contents/Frameworks/libATSCompat.dylib" ]]; then
    fail "$SRC is already patched – point --source to the original GLD Editor 1.61"
  fi
  (( FORCE )) || fail "$SRC (version '$version') is not the known GLD Editor 1.61 build.
       This patch was only tested with 1.61 Rev. 38134. Use --force to try anyway."
  print "Warning: unknown build (version '$version'), continuing because of --force"
fi
[[ "${SRC:A}" != "${DST:A}" ]] || fail "--dest must differ from --source"

# ---------------------------------------------------------------- build

fetch() { # url sha256 file
  local url=$1 sum=$2 file=$3
  if [[ ! -f "$file" ]] || [[ $(shasum -a 256 "$file" | awk '{print $1}') != "$sum" ]]; then
    curl -fsSL -o "$file" "$url"
  fi
  [[ $(shasum -a 256 "$file" | awk '{print $1}') == "$sum" ]] || fail "checksum mismatch for $file"
}

# Sets compatibility_version in LC_ID_DYLIB (old libarchive needs libxml2 >= 10.0.0)
set_compat_version() { # dylib major
  python3 -I - "$1" "$2" <<'EOF'
import struct, sys
path, major = sys.argv[1], int(sys.argv[2])
d = bytearray(open(path, 'rb').read())
ncmds = struct.unpack_from('<I', d, 16)[0]
off = 32
for _ in range(ncmds):
    cmd, size = struct.unpack_from('<II', d, off)
    if cmd == 0xD:  # LC_ID_DYLIB
        struct.pack_into('<I', d, off + 20, major << 16)
    off += size
open(path, 'wb').write(d)
EOF
}

step "Building compatibility libraries (x86_64)"
mkdir -p "$BUILD/downloads" "$BUILD/src" "$OUT"

# bzip2 1.0.8
if [[ ! -f "$OUT/libbz2.1.0.dylib" ]]; then
  print "  libbz2"
  fetch $BZIP2_URL $BZIP2_SHA256 "$BUILD/downloads/bzip2-1.0.8.tar.gz"
  rm -rf "$BUILD/src/bzip2-1.0.8"
  tar -xzf "$BUILD/downloads/bzip2-1.0.8.tar.gz" -C "$BUILD/src"
  ( cd "$BUILD/src/bzip2-1.0.8" &&
    clang $CFLAGS -D_FILE_OFFSET_BITS=64 -w -dynamiclib \
      -install_name @executable_path/../Frameworks/libbz2.1.0.dylib \
      -compatibility_version 1.0.0 -current_version 1.0.8 \
      -o "$OUT/libbz2.1.0.dylib" \
      blocksort.c huffman.c crctable.c randtable.c compress.c decompress.c bzlib.c )
fi

# libxml2 2.9.14 (only the reader API is used, by the bundled libarchive)
if [[ ! -f "$OUT/libxml2.2.dylib" ]]; then
  print "  libxml2 (takes a minute)"
  fetch $LIBXML2_URL $LIBXML2_SHA256 "$BUILD/downloads/libxml2-2.9.14.tar.xz"
  rm -rf "$BUILD/src/libxml2-2.9.14"
  tar -xJf "$BUILD/downloads/libxml2-2.9.14.tar.xz" -C "$BUILD/src"
  ( cd "$BUILD/src/libxml2-2.9.14" &&
    ./configure --host=x86_64-apple-darwin CC="clang $CFLAGS" \
      --without-python --without-lzma --without-icu --without-http --without-ftp \
      --enable-shared --disable-static >"$BUILD/libxml2-configure.log" 2>&1 &&
    make -j"$(sysctl -n hw.ncpu)" libxml2.la >"$BUILD/libxml2-make.log" 2>&1 ) ||
    fail "libxml2 build failed, see $BUILD/libxml2-*.log"
  cp "$BUILD/src/libxml2-2.9.14/.libs/libxml2.2.dylib" "$OUT/libxml2.2.dylib"
  install_name_tool -id @executable_path/../Frameworks/libxml2.2.dylib "$OUT/libxml2.2.dylib"
  set_compat_version "$OUT/libxml2.2.dylib" 10
fi

# Own libraries (always rebuilt, they are small)
print "  libcrypto.0.9.8 replacement"
clang $CFLAGS -Wall -fvisibility=hidden -dynamiclib \
  -install_name @executable_path/../Frameworks/libcrypto.0.9.8.dylib \
  -compatibility_version 0.9.8 -current_version 0.9.8 \
  -o "$OUT/libcrypto.0.9.8.dylib" "$HERE/src/libcrypto-shim.c"

print "  libGLDWindow (full screen, scaling)"
clang $CFLAGS -Wall -fobjc-arc -dynamiclib \
  -install_name @executable_path/../Frameworks/libGLDWindow.dylib \
  -framework Cocoa -o "$OUT/libGLDWindow.dylib" "$HERE/src/libGLDWindow.m"

print "  libATSCompat (font loading on macOS 27)"
clang $CFLAGS -Wall -fvisibility=hidden -dynamiclib \
  -install_name @executable_path/../Frameworks/libATSCompat.dylib \
  -compatibility_version 1.0.0 -current_version 66.0.0 \
  -framework CoreText -framework CoreGraphics -framework CoreFoundation \
  -Wl,-reexport_framework,ApplicationServices -L"$OUT" -lGLDWindow \
  -o "$OUT/libATSCompat.dylib" "$HERE/src/libATSCompat.c"

print "  libGLDHiDPI (sharp Retina rendering, text fitting)"
clang $CFLAGS -Wall -fvisibility=hidden -dynamiclib \
  -install_name @executable_path/../Frameworks/libGLDHiDPI.dylib \
  -compatibility_version 4.8.0 -current_version 4.8.5 \
  -Wl,-reexport_library,"$SRC/Contents/Frameworks/QtGui.framework/Versions/4/QtGui" \
  -o "$OUT/libGLDHiDPI.dylib" "$HERE/src/libGLDHiDPI.c"

# ---------------------------------------------------------------- patch

step "Copying GLD Editor to $DST"
if [[ -e "$DST" ]]; then
  pgrep -f "${DST}/Contents/MacOS/" >/dev/null && fail "the patched GLD Editor is running – quit it first"
  rm -rf "$DST"
fi
mkdir -p "${DST:h}"
ditto "$SRC" "$DST"
FW="$DST/Contents/Frameworks"
MAIN="$DST/Contents/MacOS/GLD Editor"

# Qt's WLAN bearer plug-in registers for CoreWLAN notifications; on macOS 27 one
# of them crashes the app (doesNotRecognizeSelector). Qt falls back to the
# generic bearer plug-in, which is all the editor needs.
rm -f "$DST/Contents/PlugIns/bearer/libqcorewlanbearer.dylib"

step "Applying compatibility libraries"
BUNDLED=(libbz2.1.0.dylib libcrypto.0.9.8.dylib libxml2.2.dylib)
cp "$OUT"/*.dylib "$FW/"
chmod 755 "$FW"/*.dylib

MACHOS=("${(@f)$(find "$DST/Contents" -type f -exec sh -c 'file "$1" | grep -q Mach-O && echo "$1"' _ {} \;)}")

# System libraries that are gone or at risk -> bundled copies;
# libgcc_s is only needed for _Unwind_Resume, which libSystem provides
for f in $MACHOS; do
  [[ "${f:h}" == "$FW" && ${BUNDLED[(Ie)${f:t}]} -gt 0 ]] && continue
  args=()
  for l in $BUNDLED; do
    otool -L "$f" | grep -q "/usr/lib/$l" && args+=(-change "/usr/lib/$l" "@executable_path/../Frameworks/$l")
  done
  otool -L "$f" | grep -q "/usr/lib/libgcc_s.1.dylib" && args+=(-change /usr/lib/libgcc_s.1.dylib /usr/lib/libSystem.B.dylib)
  if (( ${#args} )); then
    install_name_tool $args "$f" 2>/dev/null
  fi
done

# QtGui: ATS font functions (disabled in macOS 27) -> CoreText replacement
install_name_tool -change \
  /System/Library/Frameworks/ApplicationServices.framework/Versions/A/ApplicationServices \
  @executable_path/../Frameworks/libATSCompat.dylib \
  "$FW/QtGui.framework/Versions/4/QtGui" 2>/dev/null

# Main executable: QtGui -> libGLDHiDPI (re-exports QtGui)
install_name_tool -change \
  @executable_path/../Frameworks/QtGui.framework/Versions/4/QtGui \
  @executable_path/../Frameworks/libGLDHiDPI.dylib \
  "$MAIN" 2>/dev/null

# QTKit and AGL are linked but unused by QtWebKit/QtOpenGL – make them optional
for f in $MACHOS; do
  if otool -L "$f" | grep -qE "QTKit|AGL.framework"; then
    python3 -I "$HERE/tools/weaklink.py" "$f" QTKit.framework AGL.framework >/dev/null
  fi
done

/usr/libexec/PlistBuddy -c "Add :NSHighResolutionCapable bool true" "$DST/Contents/Info.plist" 2>/dev/null ||
  /usr/libexec/PlistBuddy -c "Set :NSHighResolutionCapable true" "$DST/Contents/Info.plist"

# Deliberately not code-signed: the original is unsigned too, and x86_64 code
# runs unsigned under Rosetta. Remove quarantine from everything we copied in.
xattr -cr "$DST"

otool -L "$MAIN" | grep -q libGLDHiDPI || fail "patching the main executable failed"

# ---------------------------------------------------------------- test

if (( TEST )); then
  step "Test launch (25 s)"
  cd /
  arch -x86_64 "$MAIN" >"$BUILD/test-run.log" 2>&1 &
  pid=$!
  for _ in {1..25}; do sleep 1; kill -0 $pid 2>/dev/null || break; done
  if kill -0 $pid 2>/dev/null; then
    print "  GLD Editor is running – quit it normally when you are done looking."
  else
    wait $pid; rc=$?
    # After an unclean exit the next start only cleans up and quits with 0
    (( rc == 0 )) && print "  The app exited with code 0. Start it once more – this happens after a previous crash." \
                  || fail "the app exited with code $rc, see $BUILD/test-run.log and ~/Library/Logs/DiagnosticReports"
  fi
fi

step "Done"
print "Patched app: $DST"
print "Full screen: green window button or Control-Command-F."
