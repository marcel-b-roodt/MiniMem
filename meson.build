project('minimem', 'c',
    version : '0.1.0',
    default_options : [
        'c_std=c11',
        'warning_level=2',
        'werror=true',
    ],
)

cc = meson.get_compiler('c')

zstd_dep = dependency('libzstd', required : true)

minimem_lib = static_library('minimem',
    sources : [
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
        'src/lib/advisor.c',
        'src/lib/vendor/lz4/lz4.c',
        'src/lib/vendor/lz4/lz4hc.c',
    ],
    include_directories : [include_directories('src')],
    c_args : ['-DMINIMEM_BUILD'],
    dependencies : [zstd_dep],
    install : false,
)

minimem_dep = declare_dependency(
    link_with : minimem_lib,
    include_directories : include_directories('src'),
    dependencies : [zstd_dep],
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
        dependencies : [minimem_dep, criterion_dep],
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