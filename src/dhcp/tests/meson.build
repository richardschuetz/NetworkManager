test_units = [
  'test-dhcp-dhclient',
  'test-dhcp-utils'
]

foreach test_unit: test_units
  exe = executable(
    test_unit,
    test_unit + '.c',
    dependencies: test_nm_dep,
    c_args: '-DTESTDIR="@0@"'.format(meson.current_source_dir())
  )

  test(
    'dhcp/' + test_unit,
    test_script,
    args: test_args + [exe.full_path()]
  )
endforeach
