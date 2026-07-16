# onnx-plugin-link.sh — sourced by an ONNX plugin's build.conf to set PLUGIN_BUILD_CFLAGS/LIBS for
# linking ONNX Runtime PORTABLY across Linux / Windows (mingw) / macOS. A plugin's build.conf is a tiny
# `. scripts/onnx-plugin-link.sh` so the cross-platform link logic lives in ONE place, not copied into
# every ONNX plugin (2d-3d, faceswap, voice-changer).
#
# Inputs (from the sourcing build — the Makefile PLUGIN_RULE / scripts/build-plugins.sh export these):
#   ONNX_PREFIX  dependency prefix with include/ + lib/ (Linux prebuilt, WIN_DEPS, OSX_DEPS, android deps).
#                Defaults to the repo's Linux prebuilt so a plain `make` links it.
#   PLUGIN_CC    the target compiler, used only for `-dumpmachine` to pick the platform link convention.
# If no ONNX is found the plugin's AI/RVC/depth path stubs out (BSDR_HAVE_ONNX stays unset).

: "${ONNX_PREFIX:=third_party/onnxruntime/linux-x64}"
if [ -d "$ONNX_PREFIX/include" ]; then
  PLUGIN_BUILD_CFLAGS="-DBSDR_HAVE_ONNX=1 -I$ONNX_PREFIX/include"
  _onnxlib="$(cd "$ONNX_PREFIX/lib" 2>/dev/null && pwd)"
  case "$(${PLUGIN_CC:-cc} -dumpmachine 2>/dev/null)" in
    *mingw*|*windows*|*cygwin*)
      # Windows: no import lib in WIN_DEPS, so link the DLL directly (as the core does). The DLL ships
      # beside the agent/plugin, so no rpath.
      PLUGIN_BUILD_LIBS="-L$ONNX_PREFIX/lib -l:onnxruntime.dll" ;;
    *apple*|*darwin*)
      # macOS: dylib + loader-relative rpaths so a plugin finds libonnxruntime beside the agent, in the
      # .app's Frameworks, or next to itself.
      PLUGIN_BUILD_LIBS="-L$ONNX_PREFIX/lib -lonnxruntime -Wl,-rpath,@loader_path -Wl,-rpath,@loader_path/../lib -Wl,-rpath,@loader_path/../Frameworks" ;;
    *)
      # Linux (+ Android, same ELF convention): shared lib + $ORIGIN rpaths, install libdir first (so an
      # installed plugin uses the copy in <libdir>, reached via $ORIGIN/../..), the build-tree abspath last.
      PLUGIN_BUILD_LIBS="-L$ONNX_PREFIX/lib -lonnxruntime -Wl,-rpath,\$ORIGIN -Wl,-rpath,\$ORIGIN/../lib -Wl,-rpath,\$ORIGIN/../..${_onnxlib:+ -Wl,-rpath,$_onnxlib}" ;;
  esac
elif command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libonnxruntime 2>/dev/null; then
  # System ONNX (no prefix dir) — parity with configure's system-ORT path: the loader already finds it.
  PLUGIN_BUILD_CFLAGS="-DBSDR_HAVE_ONNX=1 $(pkg-config --cflags libonnxruntime)"
  PLUGIN_BUILD_LIBS="$(pkg-config --libs libonnxruntime)"
fi
