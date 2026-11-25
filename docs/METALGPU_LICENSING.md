# MetalGPU licensing review

| Source | Reviewed notice and decision |
| --- | --- |
| WindowsiOS-ez | GPL-2.0-or-later project with existing third-party notices. New device and tests carry GPL-2.0-or-later and use the public VirtIO specification. |
| UTM | Apache-2.0 application license. Its dependency scripts point to independently licensed QEMU, virglrenderer and DXMT. |
| UTM QEMU | Reference source for PCI shared-memory mappings and fence integration. No QEMU source or CPU engine is imported. |
| viogpu-wddm | BSD-style three-clause terms at the root and in `VirtIO` and `viogpudo`. The upstream ZIPs are fetched for compatibility testing. Keep all notices and confirm the distribution terms of every bundled third-party binary before packaging the ZIP or any portion of it. |
| Neptune | Apache-2.0 protocol repository. Carry its copyright and license with the generator, transport header or generated material when shipped as required. |
| virglrenderer | MIT-style root COPYING. Neptune includes Apache-2.0 protocol material. Audit the exact included files and optional dependencies when producing an iOS build. |
| DXMT | `LICENSE` at the pinned revision is LGPL-2.1-or-later. `LICENSE.OLD` records historical MIT terms. Follow the current file-level terms, provide corresponding source for covered code and preserve the applicable replacement or relinking rights when packaging. Review the LLVM and native embedder dependencies separately. |
| Madeira | GPL-3.0-or-later with a Metal Shader Converter exception documented in `LICENSE-EXCEPTION.md` for covered Madeira contributions. The exception identifies exclusions for third-party material. Bound the extraction to the actual Madeira files included and preserve their notices. |
| Apple Metal Shader Converter | Separate proprietary binary agreement and Apache-2.0 header notices. The pinned upstream audit quotes a distribution grant for dynamic libraries used solely for shader conversion, with conditions. Verify the actual package/version, its grant and notices, native iPhoneOS support, and compatibility with every linked work before packaging. |
| Windows Driver Samples | Microsoft Public License. Consult `video/KMDOD` as a reference. Source redistribution has MS-PL terms. No sample code is imported. |

Repository URLs and revisions are recorded in [the architecture audit](METALGPU_ARCHITECTURE.md). The current branch adds no copied third-party graphics source, Windows driver packages or shader converter binary.

If Apache-2.0 or GPLv3 source later becomes part of a combined distribution, audit the existing dependency tree and GPL-2.0-or-later grants for the GPLv3 distribution route. Apache-2.0 is incompatible in a GPLv2-only combination. A license exception granted by Madeira contributors covers the code identified by that exception and cannot replace Apple's separate binary terms. For any later IPA packaging, document the distribution method, source offer, applicable notice files and LGPL compliance before public release.
