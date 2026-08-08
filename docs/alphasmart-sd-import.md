# AlphaSmart Document Storage on microSD

Each AlphaSmart document is saved independently after it has been retrieved and converted to UTF-8 text. The import destination is:

```text
/sdcard/alphasmart/slot-<file-index>_<safe-file-name>.txt
```

For example, AlphaSmart file index `3` named `Field Notes` becomes:

```text
/sdcard/alphasmart/slot-03_Field_Notes.txt
```

The index prevents collisions when two AlphaSmart files use the same display name. Import writes first to a temporary file and renames it only after the complete content is synced, preserving the prior completed copy if power is lost.

The current firmware provides the storage operation and expects UTF-8 text. The direct AlphaSmart USB transport and character-map conversion will be added only after hardware interoperability tests; NeoTools is the reference for that conversion behaviour.