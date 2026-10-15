# Third-party notices

dolphinsf2 is under the MIT licence (`LICENSE`). The libraries it builds on are under the MIT licence too, apart from
one file of ntdlib. That's `src/disc/cp932.h`, a table made from Unicode's data, under the Unicode License v3 (see
"Data from Unicode" below).

| Component | Licence | Use in dolphinsf2 |
|---|---|---|
| seqlib, Copyright (c) 2026 the seqlib authors | MIT (`externals/seqlib/LICENSE`) | In `externals/seqlib`, a submodule, compiled into dolphinsf2: the MIDI, SoundFont 2 and SFZ files and their WAV samples. |
| xsflib, Copyright (c) 2026 the xsflib authors | MIT (`externals/xsflib/LICENSE`) | In `externals/xsflib`, a submodule, compiled into dolphinsf2: bounds-checked reads in either byte order, file input and output with UTF-8 paths, the printable form of names, the console's UTF-8 arguments and output, the Windows version resource and the macOS app's droplet script. |
| ntdlib, Copyright (c) 2026 the ntdlib authors | MIT (`externals/ntdlib/LICENSE`) | In `externals/ntdlib`, a submodule, compiled into dolphinsf2: the disc reader, the readers of JAudio2's sound data, the wave decoders, the main loop's rules, the steps of a conversion and the command line's frame. |
| The code page 932 table in ntdlib's `src/disc/cp932.h`, made from Unicode's CP932.TXT, Copyright © 1991-2026 Unicode, Inc. | Unicode License v3 (`Unicode-3.0`) | Compiled into dolphinsf2 with ntdlib's disc reader. The reader decodes the Shift-JIS text of GameCube discs with it. |

Binary distributions must include the MIT notices of dolphinsf2, seqlib, xsflib and ntdlib, and the Unicode License
v3's notice. Shipping `LICENSE` and this file does that. The licence texts follow.

## Data from Unicode

ntdlib's `src/disc/cp932.h` maps the double-byte characters of code page 932, Windows' Shift-JIS, to Unicode, by their
codes. ntdlib's disc reader decodes the Shift-JIS text of GameCube discs with it. ntdlib's `tools/write_cp932_table.py`
derives the table from CP932.TXT, Microsoft's table of code page 932, table version 2.01. Unicode publishes that file.
Unicode's terms of use put Unicode's data files under the Unicode License v3. The licence's copyright and permission
notice must appear with all copies, or in the documentation that goes with them. The notice is at the end of this file.

## Console data

Nothing in dolphinsf2 licenses game data. The sound archives that dolphinsf2 reads belong to their copyright holders.

ntdlib's `include/ntdlib/nwsnd/tables.h` holds the numeric tables of NintendoWare's sound libraries: powers of two,
decibel levels, envelope attack multipliers, pan curves, low-pass cutoffs, biquad filter coefficients and sines. The
models use the exact values to reproduce the consoles' output. ntdlib's `src/sequence_export/filter_tables_*.cpp` hold
each console's filters fitted to SoundFont 2 and SFZ settings.

## Licence texts

### seqlib

```
MIT License

Copyright (c) 2026 the seqlib authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### xsflib

```
MIT License

Copyright (c) 2026 the xsflib authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### ntdlib

```
MIT License

Copyright (c) 2026 the ntdlib authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### Unicode License v3

```
UNICODE LICENSE V3

COPYRIGHT AND PERMISSION NOTICE

Copyright © 1991-2026 Unicode, Inc.

NOTICE TO USER: Carefully read the following legal agreement. BY
DOWNLOADING, INSTALLING, COPYING OR OTHERWISE USING DATA FILES, AND/OR
SOFTWARE, YOU UNEQUIVOCALLY ACCEPT, AND AGREE TO BE BOUND BY, ALL OF THE
TERMS AND CONDITIONS OF THIS AGREEMENT. IF YOU DO NOT AGREE, DO NOT
DOWNLOAD, INSTALL, COPY, DISTRIBUTE OR USE THE DATA FILES OR SOFTWARE.

Permission is hereby granted, free of charge, to any person obtaining a
copy of data files and any associated documentation (the "Data Files") or
software and any associated documentation (the "Software") to deal in the
Data Files or Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, and/or sell
copies of the Data Files or Software, and to permit persons to whom the
Data Files or Software are furnished to do so, provided that either (a)
this copyright and permission notice appear with all copies of the Data
Files or Software, or (b) this copyright and permission notice appear in
associated Documentation.

THE DATA FILES AND SOFTWARE ARE PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF
THIRD PARTY RIGHTS.

IN NO EVENT SHALL THE COPYRIGHT HOLDER OR HOLDERS INCLUDED IN THIS NOTICE
BE LIABLE FOR ANY CLAIM, OR ANY SPECIAL INDIRECT OR CONSEQUENTIAL DAMAGES,
OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THE DATA
FILES OR SOFTWARE.

Except as contained in this notice, the name of a copyright holder shall
not be used in advertising or otherwise to promote the sale, use or other
dealings in these Data Files or Software without prior written
authorization of the copyright holder.
```
