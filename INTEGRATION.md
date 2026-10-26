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

`ci/integrate-into-wine.sh <driver-dir> <wine-tree>` performs steps 1–2 (minus the
cosmetic warning guard) on a clean Wine 11 tree — teldommm/proton-wine `proton_11.0-2` —
before `autogen.sh`. `dlls/mmdevapi/main.c` is left untouched: the host app selects the
driver via the registry.

## Wine 9 / Proton 9 (arm64ec)

The same `directaudio.c` builds against Wine 9.0 (`teldommm/wine`, branch
`proton-9.0-arm64ec`). Wine 9's mmdevapi differs from Wine 11 in three ways, all handled
by the `wine9/` overlay:

| difference | Wine 9 | Wine 11 |
|---|---|---|
| PE module | each driver ships its own small `mmdevdrv.c` + `.spec` | generic, nothing to ship |
| unixlib vtable | 36 slots, **no** `midi_get_driver` | 37 slots, `midi_get_driver` at 30 |
| `is_format_supported` call | still carries `fmt_out` | no `fmt_out` in the wow64 mirror |

`wine9/Makefile.in` therefore defines `WINE_MMDEVAPI_NO_MIDI_GET_DRIVER` and
`WINE_MMDEVAPI_IS_FORMAT_SUPPORTED_FMT_OUT` for the unixlib. `wine9/mmdevdrv.c` is
`dlls/winepulse.drv/mmdevdrv.c` from the same tree, renamed (MIDI is forwarded to
`winealsa.drv`, as winepulse does).

```sh
DA_WINE_MAJOR=9 ci/integrate-into-wine.sh <driver-dir> <wine-9-tree>
```

then `autogen.sh` and the tree's own `build-scripts/build-step-arm64ec.sh --configure`
as usual; `.github/workflows/ci-wine9.yml` does exactly this and builds only the driver.
To ship it inside a full Proton 9 build, run the same integration command before the
`autogen.sh` step of that build — no other change is needed.

A Wine 9 driver is **not** interchangeable with the Wine 10 / 11 builds: the unixlib
vtable and the wow64 structs differ, so each Wine major needs its own set.
