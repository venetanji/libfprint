
sigfm_sources = ['sigfm.cpp']

opencv = dependency('opencv4', required: false)
have_sigfm = opencv.found()

if have_sigfm
  libsigfm = static_library('sigfm',
          sigfm_sources,
          dependencies: [opencv],
          cpp_args: cpp.get_supported_arguments([
              '-Wno-suggest-attribute=format',
          ]),
  )

  doctest = dependency('doctest', required: false)
  if doctest.found()
    sigfm_tests = executable('sigfm-tests', ['./tests.cpp'], dependencies: [doctest, opencv], link_with: [libsigfm])
  endif
endif
