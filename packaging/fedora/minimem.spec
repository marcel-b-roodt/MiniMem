Name:           minimem
Version:        0.6.0
Release:        1%{?dist}
Summary:        Transparent lossless memory compression library

License:        GPL-2.0-only AND BSD-2-Clause AND BSD-3-Clause
URL:            https://github.com/marcel-b-roodt/MiniMem
Source0:        %{url}/archive/refs/tags/v%{version}.tar.gz#/minimem-%{version}.tar.gz
Source1:        minimem-rpmlintrc

BuildRequires:  meson
BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  libzstd-devel

%description
MiniMem provides lossless compression algorithms optimised for memory pages,
AI weights, and structured data. This metapackage pulls in the library
and DKMS kernel module.

%package -n libminimem0
Summary:        MiniMem compression library
License:        GPL-2.0-only AND BSD-3-Clause
Requires:       libzstd%{?_isa}

%description -n libminimem0
Shared library providing lossless compression algorithms optimised for
memory pages, AI weights, and structured data.

%post -n libminimem0 -p /sbin/ldconfig
%postun -n libminimem0 -p /sbin/ldconfig

%package -n libminimem-devel
Summary:        Development files for MiniMem compression library
License:        GPL-2.0-only AND BSD-3-Clause
Requires:       libminimem0%{?_isa} = %{version}-%{release}
Requires:       libzstd-devel%{?_isa}

%description -n libminimem-devel
Headers, static library, and pkg-config file for MiniMem compression library.

%package -n minimem-dkms
Summary:        MiniMem kernel module (DKMS)
License:        GPL-2.0-only
Requires:       dkms
Requires:       gcc
Requires:       kernel-devel
BuildArch:      noarch
ExclusiveArch:  x86_64 aarch64

%description -n minimem-dkms
Transparent in-memory page compression kernel module.
Automatically rebuilds when your kernel updates.
Kernel patches for full scanner functionality are included
but require manual application and kernel rebuild.

%prep
%autosetup -n MiniMem-%{version}

%build
%meson -Dtests=false
%meson_build

%install
%meson_install

DKMS_DIR=%{buildroot}/usr/src/minimem-%{version}
mkdir -p $DKMS_DIR/lib/compressors $DKMS_DIR/include $DKMS_DIR/patches

install -m 644 dkms/dkms.conf $DKMS_DIR/
install -m 644 dkms/Makefile $DKMS_DIR/
install -m 755 dkms/install.sh $DKMS_DIR/
install -m 755 dkms/uninstall.sh $DKMS_DIR/

install -m 644 src/kernel/*.c $DKMS_DIR/
install -m 644 src/kernel/*.h $DKMS_DIR/

install -m 644 src/lib/minimem.h $DKMS_DIR/lib/
install -m 644 src/lib/advisor.c $DKMS_DIR/lib/
install -m 644 src/lib/advisor.h $DKMS_DIR/lib/

for comp in same_page bdi wkdm wkdm64 block_class lz4_wrap delta; do
    install -m 644 src/lib/compressors/$comp.* $DKMS_DIR/lib/compressors/
done

install -m 644 patches/minimem-*.patch $DKMS_DIR/patches/ || true

for stub in string.h stdbool.h stdint.h stddef.h stdlib.h limits.h; do
    install -m 644 /dev/null "$DKMS_DIR/include/$stub"
done

cat > "$DKMS_DIR/include/string.h" << 'STUB'
#ifndef _STRING_H
#define _STRING_H
#include <linux/string.h>
#endif
STUB
cat > "$DKMS_DIR/include/stdbool.h" << 'STUB'
#ifndef __STDBOOL_H
#define __STDBOOL_H
#define bool _Bool
#define true 1
#define false 0
#define __bool_true_false_are_defined 1
#endif
STUB
cat > "$DKMS_DIR/include/stdint.h" << 'STUB'
#ifndef _STDINT_H
#define _STDINT_H
#include <linux/types.h>
typedef u8 uint8_t;
typedef u16 uint16_t;
typedef u32 uint32_t;
typedef u64 uint64_t;
typedef s8 int8_t;
typedef s16 int16_t;
typedef s32 int32_t;
typedef s64 int64_t;
#endif
STUB
cat > "$DKMS_DIR/include/stddef.h" << 'STUB'
#ifndef _STDDEF_H
#define _STDDEF_H
#include <linux/stddef.h>
#endif
STUB
cat > "$DKMS_DIR/include/stdlib.h" << 'STUB'
#ifndef _STDLIB_H
#define _STDLIB_H
#include <linux/slab.h>
#include <linux/kernel.h>
#define malloc(x) kmalloc(x, GFP_KERNEL)
#define calloc(n, s) kcalloc(n, s, GFP_KERNEL)
#define free(x) kfree(x)
#endif
STUB
cat > "$DKMS_DIR/include/limits.h" << 'STUB'
#ifndef _LIMITS_H
#define _LIMITS_H
#include <linux/limits.h>
#include <linux/kernel.h>
#ifndef CHAR_BIT
#define CHAR_BIT 8
#endif
#ifndef SIZE_MAX
#define SIZE_MAX (~(size_t)0)
#endif
#endif
STUB

%post -n minimem-dkms
dkms add minimem/%{version} 2>/dev/null || true
dkms build minimem/%{version} -k $(uname -r) 2>/dev/null || true
dkms install minimem/%{version} -k $(uname -r) 2>/dev/null || true

%preun -n minimem-dkms
dkms remove minimem/%{version} --all 2>/dev/null || true

%files -n libminimem0
%license LICENSE
%{_libdir}/libminimem.so.*

%files -n libminimem-devel
%{_includedir}/minimem/
%{_libdir}/libminimem.so
%{_libdir}/libminimem_static.a
%{_libdir}/pkgconfig/minimem.pc

%files -n minimem-dkms
%attr(755,root,root) /usr/src/minimem-%{version}/install.sh
%attr(755,root,root) /usr/src/minimem-%{version}/uninstall.sh
/usr/src/minimem-%{version}/

%changelog
* Tue Jun 17 2026 Marcel Broodt <minimem@noreply.github.com> - 0.6.0-1
- Initial package