Name:           minimem
Version:        0.9.0
Release:        1%{?dist}
Summary:        Transparent lossless memory compression library

License:        GPL-2.0-only AND BSD-2-Clause AND BSD-3-Clause
URL:            https://github.com/marcel-b-roodt/MiniMem
Source0:        %{url}/archive/refs/tags/v%{version}.tar.gz#/minimem-%{version}.tar.gz
Source1:        minimem-rpmlintrc

BuildRequires:  meson
%if 0%{?fedora} || 0%{?rhel}
BuildRequires:  pkgconf-pkg-config
%else
BuildRequires:  pkg-config
%endif
BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  libzstd-devel

%description
MiniMem provides lossless compression algorithms optimised for memory pages,
AI weights, and structured data. This metapackage pulls in the library,
CLI tool, and DKMS kernel module.

%package -n minimem-cli
Summary:        CLI tool for MiniMem memory compression
License:        GPL-2.0-only
Requires:       bc
Requires:       minimem-dkms

%description -n minimem-cli
Command-line tool for monitoring and configuring MiniMem transparent
memory compression. Shows stats, toggles settings, manages the module.

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
%if 0%{?suse_version}
Requires:       kernel-default-devel
%else
Requires:       kernel-devel
%endif
Requires:       systemd
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
%if 0%{?fedora} || 0%{?rhel}
%meson -Dtests=false
%meson_build
%else
mkdir -p build
meson setup build --buildtype=plain -Dtests=false
ninja -C build %{?_smp_mflags}
%endif

%install
%if 0%{?fedora} || 0%{?rhel}
%meson_install
%else
DESTDIR=%{buildroot} ninja -C build install
%endif

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

mkdir -p %{buildroot}%{_unitdir}
install -m 644 systemd/minimem-load.service %{buildroot}%{_unitdir}/
install -m 644 systemd/minimem.service %{buildroot}%{_unitdir}/
mkdir -p %{buildroot}%{_modulesloaddir}
install -m 644 systemd/modules-load.d/minimem.conf %{buildroot}%{_modulesloaddir}/

%post -n minimem-dkms
dkms add minimem/%{version} 2>/dev/null || true
dkms build minimem/%{version} -k $(uname -r) 2>/dev/null || true
dkms install minimem/%{version} -k $(uname -r) 2>/dev/null || true
%if 0%{?fedora} || 0%{?rhel}
%systemd_post minimem-load.service minimem.service
%else
%service_add_post minimem-load.service minimem.service
%endif

%preun -n minimem-dkms
%if 0%{?fedora} || 0%{?rhel}
%systemd_preun minimem.service minimem-load.service
%else
%service_del_preun minimem.service minimem-load.service
%endif
dkms remove minimem/%{version} --all 2>/dev/null || true

%postun -n minimem-dkms
%if 0%{?fedora} || 0%{?rhel}
%systemd_postun minimem-load.service minimem.service
%else
%service_del_postun minimem-load.service minimem.service
%endif

%files -n libminimem0
%license LICENSE
%{_libdir}/libminimem.so.*

%files -n libminimem-devel
%{_includedir}/minimem/
%{_libdir}/libminimem.so
%{_libdir}/libminimem_static.a
%{_libdir}/pkgconfig/minimem.pc

%files -n minimem-cli
%{_bindir}/minimem

%files -n minimem-dkms
%attr(755,root,root) /usr/src/minimem-%{version}/install.sh
%attr(755,root,root) /usr/src/minimem-%{version}/uninstall.sh
/usr/src/minimem-%{version}/
%{_unitdir}/minimem-load.service
%{_unitdir}/minimem.service
%{_modulesloaddir}/minimem.conf

* Tue Jun 23 2026 Marcel Broodt <minimem@noreply.github.com> - 0.9.0-1
- Bump version to 0.9.0

- Add per-process compression statistics
- Add local install/uninstall script
- Add AUR minimem-dkms-systemd package
- Add recovery documentation
- Shellcheck lint fixes across test scripts

* Thu Jun 18 2026 Marcel Broodt <minimem@noreply.github.com> - 0.7.0-1
- Add parallel decompression auto-detect and sysfs toggle
- Add systemd units for auto-load and auto-enable
- Add zram coexistence documentation

* Wed Jun 17 2026 Marcel Broodt <minimem@noreply.github.com> - 0.6.0-1
- Initial package