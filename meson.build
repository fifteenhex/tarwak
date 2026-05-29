project('tarwak', 'c', meson_version: '>= 1.3.2')

libarchive = dependency('libarchive')
libcjson = dependency('libcjson')
libcap = dependency('libcap')

tarwak = executable(
    'tarwak',
    'main.c',
    dependencies: [libarchive, libcjson, libcap],
)

test_files = meson.project_source_root() / 'tests' / 'files'
test_configs = meson.project_source_root() / 'tests' / 'configs'
test_config_simple = test_configs / 'simple.json'

test_output_simple = meson.project_build_root() / 'simple.tar'

# Tests that *should* fail because of missing args
test('args-none', tarwak, args: [], should_fail: true)

test(
    'args-config-only',
    tarwak,
    args: ['-i', test_config_simple],
    should_fail: true,
)

# Parser test cases that should fail because the config is wrong
test_config_missing_file = test_configs / 'missing-file.json'
test_output_dummy = meson.project_build_root() / 'dummy.tar'

badconfig_test_cases = [
    ['missing_file', test_config_missing_file, test_output_dummy],
]

foreach case : badconfig_test_cases
    test(
        case[0],
        tarwak,
        args: ['-i', case[1], '-o', case[2], '-b', test_files],
        should_fail: true,
    )
endforeach

# Normal tests
test_config_ping_sec_cap = test_configs / 'ping-sec-cap.json'
test_output_ping_sec_cap = meson.project_build_root() / 'ping-sec-cap.tar'

test_cases = [
    ['simple', test_config_simple, test_output_simple],
    ['ping_sec_cap', test_config_ping_sec_cap, test_output_ping_sec_cap],
]

foreach case : test_cases
    test(case[0], tarwak, args: ['-i', case[1], '-o', case[2], '-b', test_files])
endforeach
