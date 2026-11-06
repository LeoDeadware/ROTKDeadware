"""Shared helpers of the HD pack builders (tools/hdui/build_pack.py, tools/hdworld/build_world.py).

Upscaler (HD_ENGINE, default auto):
  esrgan   Real-ESRGAN (ncnn-vulkan) in ROTKPort/.cache/tools/realesrgan/x: the best detail; a download
           (github.com/xinntao/Real-ESRGAN, release v0.2.5.0) that needs the owner's OK (R-C7);
  metalfx  tools/hdui/upscale.swift: Apple's MetalFX spatial scaler, local, no model files;
  lanczos  tools/hdui/upscale.swift: Core Image Lanczos + unsharp mask;
  auto     esrgan when installed, else metalfx.
Either way hdfix joins colour and alpha and corrects the tones to the original afterwards.
"""
import os
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
# ROTK_ESRGAN / ROTK_HD_BIN: the DMG's HD kit (Contents/Resources/hd: Real-ESRGAN, prebuilt tools)
ESRGAN = os.environ.get("ROTK_ESRGAN") or os.path.join(
    os.environ.get("ROTK_CACHE") or os.path.join(HERE, "..", "..", ".cache"), "tools", "realesrgan", "x")
# Caches (census runs, tools, packs) live in the repo's git-ignored out/hd (owner rule 2026-11-05:
# nothing new under ~/), or $HD_CACHE.
CACHE = os.environ.get("HD_CACHE") or os.path.join(HERE, "..", "..", "out", "hd")
TOOLS = os.path.join(CACHE, "bin")


def tool(name):
    """Compiles tools/hdui/<name>.swift once (rebuilt when the source is newer); ROTK_HD_BIN: prebuilt."""
    if os.environ.get("ROTK_HD_BIN"):
        return os.path.join(os.environ["ROTK_HD_BIN"], name)
    os.makedirs(TOOLS, exist_ok=True)
    exe, src = os.path.join(TOOLS, name), os.path.join(HERE, name + ".swift")
    if not os.path.exists(exe) or os.path.getmtime(exe) < os.path.getmtime(src):
        subprocess.run(["swiftc", "-O", "-o", exe, src], check=True)
    return exe


def engine():
    e = os.environ.get("HD_ENGINE", "auto")
    if e == "auto":
        e = "esrgan" if os.path.exists(os.path.join(ESRGAN, "realesrgan-ncnn-vulkan")) else "metalfx"
    return e


def upscale4(inp, out, model="realesrgan-x4plus"):
    """Every PNG of inp -> out, 4x (the engine above). Returns the engine used."""
    e = engine()
    os.makedirs(out, exist_ok=True)
    if e == "esrgan":
        subprocess.run([os.path.join(ESRGAN, "realesrgan-ncnn-vulkan"), "-i", inp, "-o", out, "-n", model, "-m",
                        os.path.join(ESRGAN, "models"), "-f", "png"],
                       check=True, cwd=out, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    else:
        subprocess.run([tool("upscale"), inp, out, "4", e], check=True, stdout=subprocess.DEVNULL)
    return e


def find_png(h, dirs):
    for d in dirs:
        p = os.path.join(d, h + ".png")
        if os.path.exists(p):
            return p
    return None
