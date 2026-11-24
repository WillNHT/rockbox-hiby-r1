# JPEG decoder tests

Builds the real `apps/recorder/jpeg_load.c` - the decoder behind covers,
backdrops and playlist thumbnails - on the host, and feeds it files under
AddressSanitizer and the UBSan checks for divide by zero, bounds and null.

```sh
make test     # seeds and bad files, then 2000 mutations of each (~20 s)
make fuzz     # 50000 mutations of each (minutes)
```

* `seeds/` - valid files covering 4:4:4, 4:2:2, 4:2:0, greyscale, optimised
  Huffman tables and restart markers. These must decode.
* `bad/` - files that once crashed or hung the decoder, plus a progressive
  file it does not support. These only have to be refused cleanly.

Every decode runs twice, as album art (resized into 200x200) and at full size,
in a buffer exactly as large as promised; the part the scaler would own is
poisoned, so a decoder writing past its own buffer is caught too. A decode
that runs past 5 s is a hang.

A failure names the file and the mutation:

```
FAIL seeds/420.jpg mutation 621 (--dump 621 seeds/420.jpg): ...
```

`./jpeg_test --dump 621 seeds/420.jpg` writes it to `mutation.jpg`. Once fixed,
copy it into `bad/` under a name that says what was wrong with it.

`stubs/` stands in for the Rockbox headers `jpeg_load.c` includes. The resize
stub only pulls rows through the decoder; it does not scale, so the
resized bitmap is not compared.
