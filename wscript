#!/usr/bin/env python

from waflib.Build import BuildContext, CleanContext, InstallContext, UninstallContext

top = '.'
out = 'build'


def check_compiler_flag(conf, flag, lang):
	return conf.check(fragment = 'int main() { float f = 4.0; char c = f; return c - 4; }\n', execute = 0, define_ret = 0, msg = 'Checking for compiler switch %s' % flag, cxxflags = conf.env[lang + 'FLAGS'] + [flag], okmsg = 'yes', errmsg = 'no')  # the code inside fragment deliberately does an unsafe implicit cast float->char to trigger a compiler warning; sometimes, gcc does not tell about an unsupported parameter *unless* the code being compiled causes a warning


def check_compiler_flags_2(conf, cflags, ldflags, msg):
	return conf.check(fragment = 'int main() { float f = 4.0; char c = f; return c - 4; }\n', execute = 0, define_ret = 0, msg = msg, cxxflags = cflags, ldflags = ldflags, okmsg = 'yes', errmsg = 'no')  # the code inside fragment deliberately does an unsafe implicit cast float->char to trigger a compiler warning; sometimes, gcc does not tell about an unsupported parameter *unless* the code being compiled causes a warning


def add_compiler_flags(conf, env, flags, lang, compiler, uselib = ''):
	for flag in flags:
		if type(flag) == type(()):
			flag_candidate = flag[0]
			flag_alternative = flag[1]
		else:
			flag_candidate = flag
			flag_alternative = None

		if uselib:
			flags_pattern = lang + 'FLAGS_' + uselib
		else:
			flags_pattern = lang + 'FLAGS'

		if check_compiler_flag(conf, flag_candidate, compiler):
			env.prepend_value(flags_pattern, [flag_candidate])
		elif flag_alternative:
			if check_compiler_flag(conf, flag_alternative, compiler):
				env.prepend_value(flags_pattern, [flag_alternative])



def options(opt):
	opt.add_option('--enable-debug', action = 'store_true', default = False, help = 'enable debug build [default: %default]')
	opt.add_option('--with-package-name', action = 'store', default = "gstmpg123 plug-in source release", help = 'specify package name to use in plugin [default: %default]')
	opt.add_option('--with-package-origin', action = 'store', default = "Unknown package origin", help = 'specify package origin URL to use in plugin [default: %default]')
	opt.add_option('--plugin-install-path', action = 'store', default = "${PREFIX}/lib/gstreamer-1.0", help = 'where to install the plugin for GStreamer 1.0 [default: %default]')
	opt.load('compiler_c')



def configure(conf):
	import os


	conf.load('compiler_c')


	# check and add compiler flags
	if conf.env['CFLAGS'] and conf.env['LINKFLAGS']:
		check_compiler_flags_2(conf, conf.env['CFLAGS'], conf.env['LINKFLAGS'], "Testing compiler flags %s and linker flags %s" % (' '.join(conf.env['CFLAGS']), ' '.join(conf.env['LINKFLAGS'])))
	elif conf.env['CFLAGS']:
		check_compiler_flags_2(conf, conf.env['CFLAGS'], '', "Testing compiler flags %s" % ' '.join(conf.env['CFLAGS']))
	elif conf.env['LINKFLAGS']:
		check_compiler_flags_2(conf, '', conf.env['LINKFLAGS'], "Testing linker flags %s" % ' '.join(conf.env['LINKFLAGS']))
	compiler_flags = ['-Wextra', '-Wall', '-std=c99', '-pedantic', '-fPIC', '-DPIC']
	if conf.options.enable_debug:
		compiler_flags += ['-O0', '-g3', '-ggdb']
	else:
		compiler_flags += ['-O2', '-s']

	add_compiler_flags(conf, conf.env, compiler_flags, 'C', 'CC')

	# test for SSE
	sse_test_fragment = """
	  #include <xmmintrin.h>
	  __m128 testfunc(float *a, float *b) { return _mm_add_ps(_mm_loadu_ps(a), _mm_loadu_ps(b)); }

	  int main() {
	    float a = 1.0f, b = 2.0f;
	    testfunc(&a, &b);
	    return 0;
	  }
	"""
	conf.env['SSE_SUPPORTED'] = conf.check(fragment = sse_test_fragment, execute = 0, define_ret = 0, msg = 'Checking for SSE support', okmsg = 'yes', errmsg = 'no', mandatory = 0)	

	# test for alloca.h
	conf.check_c(header_name = 'alloca.h', uselib_store = 'ALLOCA', mandatory = 0)

	# test for GStreamer libraries
	conf.check_cfg(package = 'gstreamer-1.0 >= 1.0.0',       uselib_store = 'GSTREAMER',       args = '--cflags --libs', mandatory = 1)
	conf.check_cfg(package = 'gstreamer-base-1.0 >= 1.0.0',  uselib_store = 'GSTREAMER_BASE',  args = '--cflags --libs', mandatory = 1)
	conf.check_cfg(package = 'gstreamer-audio-1.0 >= 1.0.0', uselib_store = 'GSTREAMER_AUDIO', args = '--cflags --libs', mandatory = 1)
	conf.env['PLUGIN_INSTALL_PATH'] = os.path.expanduser(conf.options.plugin_install_path)
	conf.define('GST_PACKAGE_NAME', conf.options.with_package_name)
	conf.define('GST_PACKAGE_ORIGIN', conf.options.with_package_origin)
	conf.define('PACKAGE', "gstnonstreamaudio")
	conf.define('VERSION', "1.0")

	conf.recurse('ext/dumb')

	conf.write_config_header('config.h')



def build(bld):
	nonstreamaudio_source = bld.srcnode.ant_glob('gst-libs/*.c')
	bld(
		features = ['c', 'cstlib'],  # TODO: change it back to cshlib
		includes = ['.', 'gst-libs'],
		uselib = 'GSTREAMER GSTREAMER_BASE GSTREAMER_AUDIO',
		target = 'gstnonstreamaudio',
		name = 'gstnonstreamaudio',
		source = nonstreamaudio_source
	)

	bld.recurse('ext/dumb')

