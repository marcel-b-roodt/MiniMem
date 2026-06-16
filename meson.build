project('minimem', 'c', 'cpp',
    version : '0.1.0',
    default_options : [
        'c_std=c11',
        'cpp_std=c++14',
        'warning_level=2',
        'werror=true',
    ],
)

cc = meson.get_compiler('c')

zstd_dep = dependency('libzstd', required : true)

minimem_version_major = '0'
minimem_version_minor = '1'
minimem_version_patch = '0'
minimem_version = '@0@.@1@.@2@'.format(minimem_version_major, minimem_version_minor, minimem_version_patch)

minimem_lib_sources = [
    'src/lib/test_data.c',
    'src/lib/registry.c',
    'src/lib/compressors/same_page.c',
    'src/lib/compressors/bdi.c',
    'src/lib/compressors/wkdm.c',
    'src/lib/compressors/lz4_wrap.c',
    'src/lib/compressors/lzsse8.c',
    'src/lib/compressors/zstd_dict.c',
    'src/lib/compressors/delta.c',
    'src/lib/compressors/wkdm64.c',
    'src/lib/compressors/block_class.c',
    'src/lib/compressors/ai_weights.c',
    'src/lib/advisor.c',
    'src/lib/vendor/lz4/lz4.c',
    'src/lib/vendor/lz4/lz4hc.c',
]

lzsse8_cpp = files('src/lib/vendor/lzsse8/lzsse8.cpp')

lzsse8_cpp_args = ['-msse4.1', '-Wno-unused-but-set-variable', '-Wno-sign-compare', '-Wno-unused-variable', '-Wno-unknown-pragmas']

minimem_inc = include_directories('src')

lzsse8_lib = static_library('lzsse8_wrap',
    sources : lzsse8_cpp,
    include_directories : [minimem_inc],
    cpp_args : lzsse8_cpp_args,
    install : false,
)

minimem_lib = static_library('minimem_static',
    sources : minimem_lib_sources,
    include_directories : [minimem_inc],
    c_args : ['-DMINIMEM_BUILD'],
    dependencies : [zstd_dep],
    install : true,
    link_with : lzsse8_lib,
)

minimem_shared = shared_library('minimem',
    sources : minimem_lib_sources + lzsse8_cpp,
    include_directories : [minimem_inc],
    c_args : ['-DMINIMEM_BUILD'],
    cpp_args : lzsse8_cpp_args,
    dependencies : [zstd_dep],
    version : minimem_version,
    install : true,
)

minimem_dep_static = declare_dependency(
    link_with : minimem_lib,
    include_directories : minimem_inc,
    dependencies : [zstd_dep],
)

minimem_dep_shared = declare_dependency(
    link_with : minimem_shared,
    include_directories : minimem_inc,
    dependencies : [zstd_dep],
)

pkgconfig = import('pkgconfig')

pkgconfig.generate(
    libraries : minimem_lib,
    version : minimem_version,
    name : 'minimem',
    description : 'Transparent lossless memory compression library',
    subdirs : 'minimem',
    requires : 'libzstd',
)

install_headers(
    'src/lib/minimem.h',
    'src/lib/advisor.h',
    'src/lib/test_data.h',
    subdir : 'minimem',
)

criterion_dep = dependency('criterion', required : get_option('tests'))

if criterion_dep.found()
    test_exe = executable('minimem_tests',
        sources : [
            'tests/lib/test_acceptance.c',
            'tests/lib/test_lz4.c',
            'tests/lib/test_algorithms.c',
            'tests/lib/bench_harness.c',
            'tests/lib/test_data_loaders.c',
        ],
        dependencies : [minimem_dep_static, criterion_dep],
        include_directories : [include_directories('tests')],
        install : false,
    )
    test('minimem_unit_tests', test_exe)

    kernel_test_exe = executable('minimem_kernel_test',
        sources : [
            'tests/kernel/test_minimem_kernel.c',
            'src/lib/test_data.c',
            'src/lib/registry.c',
            'src/lib/compressors/same_page.c',
            'src/lib/compressors/bdi.c',
            'src/lib/compressors/wkdm.c',
            'src/lib/compressors/lz4_wrap.c',
            'src/lib/compressors/lzsse8.c',
            'src/lib/compressors/zstd_dict.c',
            'src/lib/compressors/delta.c',
            'src/lib/compressors/wkdm64.c',
            'src/lib/compressors/block_class.c',
            'src/lib/compressors/ai_weights.c',
            'src/lib/advisor.c',
            'src/lib/vendor/lz4/lz4.c',
            'src/lib/vendor/lz4/lz4hc.c',
        ],
        include_directories : [include_directories('src'), include_directories('tests')],
        dependencies : [zstd_dep],
        install : false,
    )

    bench_exe = executable('minimem_bench',
        sources : [
            'reports/bench_runner.c',
            'src/lib/test_data.c',
            'src/lib/registry.c',
            'src/lib/compressors/same_page.c',
            'src/lib/compressors/bdi.c',
            'src/lib/compressors/wkdm.c',
            'src/lib/compressors/lz4_wrap.c',
            'src/lib/compressors/lzsse8.c',
            'src/lib/compressors/zstd_dict.c',
            'src/lib/compressors/delta.c',
            'src/lib/compressors/wkdm64.c',
            'src/lib/compressors/block_class.c',
            'src/lib/compressors/ai_weights.c',
            'src/lib/advisor.c',
            'src/lib/vendor/lz4/lz4.c',
            'src/lib/vendor/lz4/lz4hc.c',
            'tests/lib/bench_harness.c',
        ],
        include_directories : [include_directories('src'), include_directories('tests')],
        dependencies : [zstd_dep],
        install : false,
    )
endif