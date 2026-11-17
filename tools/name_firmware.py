# ============================================================================
#  name_firmware.py – a PlatformIO "extra script": name the firmware properly
# ============================================================================
#
#  Without this, every ESP32 build is called `firmware.bin` / `firmware.elf`:
#  vague, and the same name for every project (try finding the right one in
#  Launcher's OTA dialog, or in a folder of downloads). With it, `pio run -e tdeck`
#  produces
#
#      .pio/build/tdeck/tdeckjisho-1.0.bin      <- upload this one
#      .pio/build/tdeck/tdeckjisho-1.0.elf      <- crash backtraces are decoded with this
#
#  Two settings in platformio.ini drive it, so the script itself is reusable by
#  copying it into another project:
#      custom_name    = tdeckjisho      (default: firmware)
#      custom_version = 1.0            (default: dev)
#  The version lives ONLY there. The same
#  string is also compiled into the program (JISHO_VERSION, shown on the About
#  screen), so the file name and the screen can never disagree.
#
#  Hooked in by platformio.ini:   extra_scripts = pre:tools/name_firmware.py
#
#  How it works: PlatformIO builds every output file name from the SCons variable
#  PROGNAME ("program name"; Arduino-ESP32 sets it to "firmware" only if nobody
#  chose one before). `pre:` means this script runs BEFORE the platform's own build
#  script, so our choice wins, and every later step (linking, the .bin, upload, the
#  serial monitor's exception decoder) follows it without further changes.
#
#  This is not an ordinary Python module: SCons gives it a global `env` object
#  through the special Import() call below. Your editor will flag `Import` and `env`
#  as undefined. That is normal; every PlatformIO extra script looks like this.
# ============================================================================

Import("env")  # noqa: F821  (injected by SCons)

name = env.GetProjectOption("custom_name", "firmware")
version = env.GetProjectOption("custom_version", "dev")

# Compile the version into the program: -DJISHO_VERSION="1.0" (with the quotes).
# StringifyMacro adds the quotes correctly on every operating system.
env.Append(CPPDEFINES=[("JISHO_VERSION", env.StringifyMacro(version))])

# Rename the outputs, but only for the device. The simulator's program stays
# `program`, because the docs, the key-script examples and the tests refer to it.
if env["PIOENV"].startswith("tdeck"):
    env.Replace(PROGNAME="%s-%s" % (name, version))
    print("[name_firmware] output: %s-%s.bin" % (name, version))
