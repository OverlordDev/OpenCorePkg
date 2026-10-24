# TestPickerEditor

Runs the real picker options editor (`Library/OcMainLib/OpenCoreEditor*.c`, `OpenCoreEditPlist.c`
and the boot-args code of `OpenCoreNvram.c`) on the host, driven by scripted key presses:

- a virtual 80x25 console, which also fails the test if the editor writes to the last
  column or row, or scrolls the screen;
- a variable store which behaves like UEFI NVRAM (attributes, flash vs. volatile, resets);
- a directory of the host in place of the OpenCore storage, with `config.plist` taken from
  `Docs/Sample.plist`.

It checks editing of boot arguments (for one boot, kept, saved to `config.plist`), restoring
of the original arguments, browsing, changing and saving `config.plist`, and choosing between
`config*.plist` files (the `config.choice` file, fallback to `config.plist`, listing of the files,
switching for one start or for good, and editing the configuration which is in use), and the kexts
screen (switching, moving with plus and minus, the check of the order against fake `Info.plist`
files that the test writes, `F5`, saving, long lists).

```
cd Utilities/TestPickerEditor
make
./PickerEditor ../../Docs/Sample.plist
```

`PICKER_TEST_DIR=<copy of EFI/OC> PICKER_TEST_KEXTS=1 ./PickerEditor ../../Docs/Sample.plist` prints the kexts screen of
the configuration in that directory, as it is and after `F5`, saving nothing (use a copy anyway).

`UDK_PATH` has to point to the UDK, `../../UDK` by default. Use `SANITIZE=1 make` for a sanitized
build. `PICKER_TEST_SHOW=1 ./PickerEditor ../../Docs/Sample.plist` prints the screens of a sample
session instead of running the checks.

This test is not part of `build_oc.tool`, as it relies on a POSIX host.
