#!/bin/sh
# Makes a macOS app bundle from a built executable: the program and the files beside it, an
# Info.plist, an icon, and a signature. Run it on a Mac (it uses sips, iconutil and codesign); the
# executable may have been built elsewhere with -Dtarget=aarch64-macos -Dmacos_sdk=PATH.
#
#   tools/bundle-macos.sh -n NAME -e EXECUTABLE -i IDENTIFIER -v VERSION [options]
#
#   -n NAME         the app's name: the bundle is OUT/NAME.app
#   -e EXECUTABLE   the program to wrap
#   -i IDENTIFIER   its bundle identifier, such as com.example.app
#   -v VERSION      its version, such as 1.2.0
#   -o OUT          where to put the bundle (the executable's folder unless given)
#   -c ICON.png     a square picture for its icon, 1024 pixels for a sharp one
#   -r PATH         a file or folder copied beside the program in Contents/MacOS (may repeat):
#                   helper programs and libraries the program finds next to itself
#   -u KEY=TEXT     a usage string macOS shows when the app first asks for something, such as
#                   NSMicrophoneUsageDescription="Records while you dictate." (may repeat). A bare
#                   executable cannot ask at all: the question needs a bundle with the string.
#   -b              a background app: no Dock icon and no menu bar, for an app that lives in the
#                   tray (LSUIElement). With SDL, also set SDL_HINT_MAC_BACKGROUND_APP to "1"
#                   before SDL_Init, or SDL shows the app in the Dock itself.
#   -m VERSION      the oldest macOS it runs on (13.0 unless given)
#   -s IDENTITY     the certificate to sign with, by its name in the keychain. Without one the
#                   signature is ad hoc. macOS files what the user has granted an app (Microphone,
#                   Accessibility) under its signature, so an ad hoc signed build loses its grants
#                   with every build and one signed with a named certificate keeps them.
set -eu
name= exe= id= version= out= icon= identity=- background= minimum=13.0 resources= usage=
while getopts n:e:i:v:o:c:r:u:bm:s: opt; do
    case $opt in
    n) name=$OPTARG ;; e) exe=$OPTARG ;; i) id=$OPTARG ;; v) version=$OPTARG ;; o) out=$OPTARG ;; c) icon=$OPTARG ;;
    r) resources="$resources$OPTARG
" ;;
    u) usage="$usage$OPTARG
" ;;
    b) background=1 ;; m) minimum=$OPTARG ;; s) identity=$OPTARG ;;
    *) sed -n '2,29p' "$0" >&2; exit 2 ;;
    esac
done
if [ -z "$name" ] || [ -z "$exe" ] || [ -z "$id" ] || [ -z "$version" ]; then sed -n '2,29p' "$0" >&2; exit 2; fi
[ -n "$out" ] || out=$(dirname "$exe")
app=$out/$name.app
program=$(basename "$exe")
escape() { printf '%s' "$1" | sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g' -e 's/>/\&gt;/g'; }

rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp "$exe" "$app/Contents/MacOS/$program"
printf '%s' "$resources" | while IFS= read -r path; do
    [ -z "$path" ] || cp -R "$path" "$app/Contents/MacOS/"
done

if [ -n "$icon" ]; then  # the picture in the sizes an icon set wants
    set=$out/$name.iconset
    rm -rf "$set"
    mkdir -p "$set"
    for size in 16 32 128 256 512; do
        sips -z $size $size "$icon" --out "$set/icon_${size}x${size}.png" >/dev/null
        sips -z $((size * 2)) $((size * 2)) "$icon" --out "$set/icon_${size}x${size}@2x.png" >/dev/null
    done
    iconutil -c icns "$set" -o "$app/Contents/Resources/$name.icns"
    rm -rf "$set"
fi

{
    cat <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>$(escape "$name")</string>
    <key>CFBundleDisplayName</key><string>$(escape "$name")</string>
    <key>CFBundleIdentifier</key><string>$(escape "$id")</string>
    <key>CFBundleExecutable</key><string>$(escape "$program")</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$(escape "$version")</string>
    <key>CFBundleVersion</key><string>$(escape "$version")</string>
    <key>LSMinimumSystemVersion</key><string>$(escape "$minimum")</string>
    <key>NSHighResolutionCapable</key><true/>
PLIST
    [ -z "$icon" ] || echo "    <key>CFBundleIconFile</key><string>$(escape "$name")</string>"
    [ -z "$background" ] || echo "    <key>LSUIElement</key><true/>"
    printf '%s' "$usage" | while IFS= read -r pair; do
        [ -z "$pair" ] || echo "    <key>$(escape "${pair%%=*}")</key><string>$(escape "${pair#*=}")</string>"
    done
    printf '</dict>\n</plist>\n'
} > "$app/Contents/Info.plist"
plutil -lint "$app/Contents/Info.plist" >/dev/null

# What is inside first, then the app around it.
find "$app/Contents/MacOS" -type f ! -name "$program" -exec codesign --force --sign "$identity" {} \;
codesign --force --sign "$identity" "$app"
codesign --verify --strict "$app"
echo "$app"
