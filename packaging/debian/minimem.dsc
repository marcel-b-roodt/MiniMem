Format: 3.0 (quilt)
Source: minimem
Maintainer: Marcel Broodt <minimem@noreply.github.com>
Section: utils
Priority: optional
Build-Depends: debhelper-compat (= 13), meson, gcc, g++, libzstd-dev, dkms
Standards-Version: 4.7.0
Homepage: https://github.com/marcel-b-roodt/MiniMem

Package: libminimem0
Architecture: any
Depends: ${shlibs:Depends}, ${misc:Depends}
Description: Transparent lossless memory compression library
 MiniMem provides lossless compression algorithms optimised for memory pages,
 AI weights, and structured data.

Package: libminimem-dev
Section: libdevel
Architecture: any
Depends: libminimem0 (= ${binary:Version}), libzstd-dev, ${misc:Depends}
Description: Transparent lossless memory compression library - development
 MiniMem provides lossless compression algorithms optimised for memory pages,
 AI weights, and structured data.
 .
 This package provides headers, static library, and pkg-config file.

Package: minimem-dkms
Architecture: all
Depends: dkms, gcc, ${misc:Depends}
Description: MiniMem kernel module (DKMS)
 Transparent in-memory page compression kernel module.
 Automatically rebuilds when your kernel updates.
 Kernel patches for full scanner functionality are included
 but require manual application and kernel rebuild.