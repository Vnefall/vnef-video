# Third-Party Notices

This project uses FFmpeg at runtime for decoding.

FFmpeg is licensed under the LGPL v2.1 or later by default. If FFmpeg is
built with GPL-only components enabled, the resulting FFmpeg binaries are
GPL and any binary that links to them must comply with the GPL.

Your responsibilities when distributing binaries that link to FFmpeg:
- Ensure your FFmpeg build configuration (LGPL-only vs GPL) matches your
  intended license obligations.
- Provide the applicable FFmpeg license text with your distribution.
- Provide access to the FFmpeg source code corresponding to the binaries
  you distribute, as required by the LGPL/GPL.

This notice is provided to help downstream users comply with FFmpeg's
license terms.
