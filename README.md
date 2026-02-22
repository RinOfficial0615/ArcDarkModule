# ArcDarkModule

Replace track images of the certain rhythm game to dark ones

## Requirements

- Android NDK >= r28
- Phone with zygisk enabled

## How to use?

- Clone the repository
- Optional: set `ANDROID_NDK_HOME` (or pass `--ndk-home`). If not set, script auto-detects from `ANDROID_SDK_ROOT` / `ANDROID_HOME`.
- Open your powershell and run `.\build.ps1 --rel`
- Push `.\build\ArcDarkModule.zip` to your phone and install it in your root manager
- Enjoy!

## Dynamic switch

- Create `<module dir>/disable` or `<module dir>/remove` to disable module logic at startup.
- Delete the flag file to re-enable on next app/process start.
