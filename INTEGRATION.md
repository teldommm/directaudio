# Integrating DirectAudio into a Wine / Proton tree

This repo is the `dlls/winedirectaudio.drv/` directory. Two integration points touch
files **outside** that directory, so they live in the **consumer** (e.g. proton-wine),
not in this submodule. They total ~10 lines and rarely change.

## 1. Add the submodule
```sh
git submodule add https://github.com/The412Banner/directaudio dlls/winedirectaudio.drv
```

## 2. `configure.ac` (Wine root)

**a.** With the other `--with-*` sound args (near `--without-alsa`):
```m4
AC_ARG_WITH(aaudio,    AS_HELP_STRING([--without-aaudio],[do not use the Android AAudio sound support]))
```

**b.** Detection block, alongside the other audio backends:
```m4
dnl **** Check for AAudio (Android native audio) for the DirectAudio driver ****
if test "x$with_aaudio" != "xno"
then
    AC_CHECK_HEADER([aaudio/AAudio.h],
        [AC_SUBST(AAUDIO_LIBS,"-laaudio")
         enable_winedirectaudio_drv=${enable_winedirectaudio_drv:-yes}])
fi
```

**c.** Default-disable line, with the other `enable_wine*_drv` defaults:
```m4
enable_winedirectaudio_drv=${enable_winedirectaudio_drv:-no}
```

**d.** Include it in the "no sound driver selected" warning guard (add
`$enable_winedirectaudio_drv` to the driver list and `$with_aaudio` to the with-list).

**e.** Register the makefile (with the other `WINE_CONFIG_MAKEFILE(dlls/wine*.drv)`):
```m4
WINE_CONFIG_MAKEFILE(dlls/winedirectaudio.drv)
```

## 3. `dlls/mmdevapi/main.c` — driver selection

`default_list` is the fallback order when `HKCU\Software\Wine\Drivers` `Audio` is unset.

- **Opt-in (recommended, matches shipped builds):** leave `default_list` unchanged
  (no `directaudio`). Users enable it explicitly:
  ```
  HKCU\Software\Wine\Drivers  "Audio" = "directaudio"
  ```
- **Default-on (dev/testing):** prepend it:
  ```c
  static WCHAR default_list[] = L"directaudio,pulse,alsa,oss,coreaudio";
  ```

## 4. Build
```sh
./configure --enable-archs=arm64ec --with-aaudio
make dlls/winedirectaudio.drv
```
Produces the hot-swap pair: `winedirectaudio.drv` (arm64ec PE) + `winedirectaudio.so` (unixlib).

## Automated integration (CI)

`ci/integrate-into-wine.sh <driver-dir> <wine-tree> <wine10|wine11>` performs steps 1–2
(minus the cosmetic warning guard) on a clean tree such as GameNative/proton-wine
`proton_10.0` / `proton_11.0-2`, before `autogen.sh`. For `wine10` it copies the PE side
from `wine10/` (`mmdevdrv.c`, `winedirectaudio.drv.spec`, and a `Makefile.in` that defines
`WINE_MMDEVAPI_NO_MIDI_GET_DRIVER`, since Wine 10's mmdevapi has no `midi_get_driver` slot).
`dlls/mmdevapi/main.c` is left untouched: the host app selects the driver via the registry.
