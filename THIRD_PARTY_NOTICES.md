# Third-party notices

The application links the following vendored dependencies. Their license
notices accompany source distributions and packaged binaries.

## Projects KnockThru adapts code from

Both projects use the MIT license below.

- [KominoVR/frame-passthrough-shortcuts](https://github.com/KominoVR/frame-passthrough-shortcuts):
  Copyright (c) 2026 Frame Passthrough Shortcuts contributors. KnockThru adapts its
  SteamVR session and install code, the Room View style switch, the build setup,
  and `scripts/package.mjs`.
- [toorux/steam-frame-passthrough-toggle](https://github.com/toorux/steam-frame-passthrough-toggle):
  Copyright (c) 2026 Steam Frame Passthrough Toggle contributors. KnockThru ports its
  debug port client and the script that finds and clicks the Room View button.

```text
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

## Compiler support libraries

The ARM64 cross-build uses Zig 0.16.0 and statically links its LLVM C++
support libraries. Their notices are included in `third_party/toolchain-licenses/`:
Zig's MIT license, and the libc++, libc++abi, and libunwind licenses. Source:
[Zig 0.16.0](https://ziglang.org/download/0.16.0/release-notes.html).
Host builds using GCC link with the GCC Runtime Library Exception.
The Linux C library and SteamVR runtime are loaded from the device, not bundled.

## OpenVR SDK

- Source: [ValveSoftware/openvr](https://github.com/ValveSoftware/openvr/tree/0924064316de3effbcd1acf1e309182a2deb1c05)
- Pinned commit: `0924064316de3effbcd1acf1e309182a2deb1c05`
- Copyright (c) 2015, Valve Corporation. All rights reserved.
- License: BSD 3-Clause. The full notice is included at
  [third_party/openvr/LICENSE](third_party/openvr/LICENSE).
- The portable loader is compiled from source. Valve's runtime is not bundled.

The SDK includes JsonCpp, used by the loader. Its embedded notice offers public
domain or MIT terms. The MIT notice is reproduced below for binary distributions.

## JSON for Modern C++

- Source: [nlohmann/json v3.11.3](https://github.com/nlohmann/json/tree/v3.11.3)
- Copyright (c) 2013-2022 Niels Lohmann.
- License: MIT. The full notice is included at
  [third_party/json/LICENSE.MIT](third_party/json/LICENSE.MIT).

## Noto Sans

- Source: [notofonts/latin-greek-cyrillic](https://github.com/notofonts/latin-greek-cyrillic), version 2.015
- Copyright 2022 The Noto Project Authors.
- License: SIL Open Font License 1.1. The full license is included at
  [assets/fonts/OFL.txt](assets/fonts/OFL.txt).
- `NotoSans-SemiBold.ttf` and `NotoSans-Regular.ttf` draw the dashboard panel text.

## stb_truetype and stb_image_write

- Source: [nothings/stb](https://github.com/nothings/stb), stb_truetype v1.26 and stb_image_write v1.16
- Copyright (c) 2017 Sean Barrett.
- License: MIT or public domain, at your choice. The notice is at the end of
  each header in `third_party/stb/`.

## Vulkan headers

- Source: [KhronosGroup/Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers), header version 1.4.357
- Copyright 2015-2026 The Khronos Group Inc.
- License: Apache-2.0 or MIT, at your choice. The MIT notice is included at
  [third_party/vulkan/MIT.txt](third_party/vulkan/MIT.txt).
- The Vulkan loader is loaded from the device at runtime, not bundled.

## JsonCpp notice from the OpenVR SDK

```text
Copyright (c) 2007-2010 Baptiste Lepilleur

Permission is hereby granted, free of charge, to any person
obtaining a copy of this software and associated documentation
files (the "Software"), to deal in the Software without
restriction, including without limitation the rights to use, copy,
modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
