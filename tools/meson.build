if get_option('kernel') == 'ish'
    if not meson.is_cross_build()
        uname_test = executable('uname-test',
            ['uname-test.c'], dependencies: [ish])
        test('uname', uname_test)
    endif

    # The differential tracers model the x86 guest CPU state.
    if guest_arch == 'x86'
        dirty_page_trace_test = executable('dirty-page-trace-test',
            ['dirty-page-trace-test.c'], dependencies: [ish])
        test('dirty-page-trace', dirty_page_trace_test)

        # ptraceomatic is additionally specific to x86_64 Linux hosts.
        if host_machine.system() == 'linux' and host_machine.cpu() == 'x86_64'
            transplant_src = [
                'vdso-transplant.c',
                'ptutil.c',
            ]
            executable('ptraceomatic', ['ptraceomatic.c', 'undefined-flags.c', transplant_src], dependencies: [ish])
            configure_file(input: 'ptraceomatic-gdb.gdb', output: '@PLAINNAME@', copy: true)

            # tools for messing with vdsos
            # executable('vdso-dump', ['vdso-dump.c'], c_args: ['-m32'], link_args: ['-m32'])
            executable('vdso-transplant', ['vdso-transplant-main.c', transplant_src],
                include_directories: includes)
        endif

        unicorn = cc.find_library('unicorn', required: false)
        if unicorn.found()
            executable('unicornomatic', ['unicornomatic.c', 'undefined-flags.c'], dependencies: [ish, unicorn])
            configure_file(input: 'ptraceomatic-gdb.gdb', output: 'unicornomatic-gdb.gdb', copy: true)
        endif
    endif

    if not meson.is_cross_build() and guest_arch == 'arm64' and host_machine.cpu_family() == 'aarch64'
        dirty_page_chain_test = executable('dirty-page-chain-test',
            ['dirty-page-chain-test.c'], dependencies: [ish])
        test('dirty-page-chain', dirty_page_chain_test)
    endif
endif

libarchive = dependency('libarchive', required: false)
if not libarchive.found()
    # homebrew
    libarchive_lib = cc.find_library('archive', dirs: ['/usr/local/opt/libarchive/lib'], required: false)
    if libarchive_lib.found()
        libarchive = declare_dependency(dependencies: [libarchive_lib], include_directories: include_directories(['/usr/local/opt/libarchive/include']))
    else
        # Apple Silicon Homebrew
        libarchive_lib = cc.find_library('archive', dirs: ['/opt/homebrew/opt/libarchive/lib'], required: false)
        if libarchive_lib.found()
            libarchive = declare_dependency(dependencies: [libarchive_lib], include_directories: include_directories(['/opt/homebrew/opt/libarchive/include']))
        endif
    endif
endif

if libarchive.found()
    fakefsify_src = [
        'fakefsify.c',
        'fakefs.c',
        '../util/fchdir.c',
    ]
    fakefsify = executable('fakefsify', fakefsify_src, dependencies: [libarchive], link_with: [libfakefs], include_directories: [includes])
    custom_target('unfakefsify',
        build_by_default: true,
        command: ['ln', '-sf', 'fakefsify', '@OUTPUT@'],
        output: 'unfakefsify')
endif
