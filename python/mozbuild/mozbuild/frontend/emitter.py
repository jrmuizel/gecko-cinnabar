# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

from __future__ import unicode_literals

import itertools
import json
import logging
import os
import traceback
import sys
import time

from collections import defaultdict, OrderedDict
from mach.mixin.logging import LoggingMixin
from mozbuild.util import (
    memoize,
    OrderedDefaultDict,
)

import mozpack.path as mozpath
import manifestparser
import reftest
import mozinfo

from .data import (
    ConfigFileSubstitution,
    ContextWrapped,
    Defines,
    DirectoryTraversal,
    Exports,
    GeneratedEventWebIDLFile,
    GeneratedInclude,
    GeneratedWebIDLFile,
    ExampleWebIDLInterface,
    ExternalStaticLibrary,
    ExternalSharedLibrary,
    HeaderFileSubstitution,
    HostLibrary,
    HostProgram,
    HostSimpleProgram,
    InstallationTarget,
    IPDLFile,
    JARManifest,
    JavaScriptModules,
    Library,
    Linkable,
    LinkageWrongKindError,
    LocalInclude,
    PerSourceFlag,
    PreprocessedTestWebIDLFile,
    PreprocessedWebIDLFile,
    Program,
    ReaderSummary,
    Resources,
    SharedLibrary,
    SimpleProgram,
    StaticLibrary,
    TestHarnessFiles,
    TestWebIDLFile,
    TestManifest,
    VariablePassthru,
    WebIDLFile,
    XPIDLFile,
)

from .reader import SandboxValidationError

from .context import Context


class TreeMetadataEmitter(LoggingMixin):
    """Converts the executed mozbuild files into data structures.

    This is a bridge between reader.py and data.py. It takes what was read by
    reader.BuildReader and converts it into the classes defined in the data
    module.
    """

    def __init__(self, config):
        self.populate_logger()

        self.config = config

        mozinfo.find_and_update_from_json(config.topobjdir)

        # Python 2.6 doesn't allow unicode keys to be used for keyword
        # arguments. This gross hack works around the problem until we
        # rid ourselves of 2.6.
        self.info = {}
        for k, v in mozinfo.info.items():
            if isinstance(k, unicode):
                k = k.encode('ascii')
            self.info[k] = v

        self._libs = OrderedDefaultDict(list)
        self._binaries = OrderedDict()
        self._linkage = []
        self._static_linking_shared = set()

        # Keep track of external paths (third party build systems), starting
        # from what we run a subconfigure in. We'll eliminate some directories
        # as we traverse them with moz.build (e.g. js/src).
        subconfigures = os.path.join(self.config.topobjdir, 'subconfigures')
        paths = []
        if os.path.exists(subconfigures):
            paths = open(subconfigures).read().splitlines()
        self._external_paths = set(mozpath.normsep(d) for d in paths)
        # Add security/nss manually, since it doesn't have a subconfigure.
        self._external_paths.add('security/nss')

    def emit(self, output):
        """Convert the BuildReader output into data structures.

        The return value from BuildReader.read_topsrcdir() (a generator) is
        typically fed into this function.
        """
        file_count = 0
        sandbox_execution_time = 0.0
        emitter_time = 0.0
        contexts = {}

        def emit_objs(objs):
            for o in objs:
                yield o
                if not o._ack:
                    raise Exception('Unhandled object of type %s' % type(o))

        for out in output:
            if isinstance(out, Context):
                # Keep all contexts around, we will need them later.
                contexts[out.objdir] = out

                start = time.time()
                # We need to expand the generator for the timings to work.
                objs = list(self.emit_from_context(out))
                emitter_time += time.time() - start

                for o in emit_objs(objs): yield o

                # Update the stats.
                file_count += len(out.all_paths)
                sandbox_execution_time += out.execution_time

            else:
                raise Exception('Unhandled output type: %s' % type(out))

        start = time.time()
        objs = list(self._emit_libs_derived(contexts))
        emitter_time += time.time() - start

        for o in emit_objs(objs): yield o

        yield ReaderSummary(file_count, sandbox_execution_time, emitter_time)

    def _emit_libs_derived(self, contexts):
        # First do FINAL_LIBRARY linkage.
        for lib in (l for libs in self._libs.values() for l in libs):
            if not isinstance(lib, StaticLibrary) or not lib.link_into:
                continue
            if lib.link_into not in self._libs:
                raise SandboxValidationError(
                    'FINAL_LIBRARY ("%s") does not match any LIBRARY_NAME'
                    % lib.link_into, contexts[lib.objdir])
            candidates = self._libs[lib.link_into]

            # When there are multiple candidates, but all are in the same
            # directory and have a different type, we want all of them to
            # have the library linked. The typical usecase is when building
            # both a static and a shared library in a directory, and having
            # that as a FINAL_LIBRARY.
            if len(set(type(l) for l in candidates)) == len(candidates) and \
                   len(set(l.objdir for l in candidates)) == 1:
                for c in candidates:
                    c.link_library(lib)
            else:
                raise SandboxValidationError(
                    'FINAL_LIBRARY ("%s") matches a LIBRARY_NAME defined in '
                    'multiple places:\n    %s' % (lib.link_into,
                    '\n    '.join(l.objdir for l in candidates)),
                    contexts[lib.objdir])

        # Next, USE_LIBS linkage.
        for context, obj, variable in self._linkage:
            self._link_libraries(context, obj, variable)

        def recurse_refs(lib):
            for o in lib.refs:
                yield o
                if isinstance(o, StaticLibrary):
                    for q in recurse_refs(o):
                        yield q

        # Check that all static libraries refering shared libraries in
        # USE_LIBS are linked into a shared library or program.
        for lib in self._static_linking_shared:
            if all(isinstance(o, StaticLibrary) for o in recurse_refs(lib)):
                shared_libs = sorted(l.basename for l in lib.linked_libraries
                    if isinstance(l, SharedLibrary))
                raise SandboxValidationError(
                    'The static "%s" library is not used in a shared library '
                    'or a program, but USE_LIBS contains the following shared '
                    'library names:\n    %s\n\nMaybe you can remove the '
                    'static "%s" library?' % (lib.basename,
                    '\n    '.join(shared_libs), lib.basename),
                    contexts[lib.objdir])

        # Propagate LIBRARY_DEFINES to all child libraries recursively.
        def propagate_defines(outerlib, defines):
            outerlib.defines.update(defines)
            for lib in outerlib.linked_libraries:
                # Propagate defines only along FINAL_LIBRARY paths, not USE_LIBS
                # paths.
                if (isinstance(lib, StaticLibrary) and
                        lib.link_into == outerlib.basename):
                    propagate_defines(lib, defines)

        for lib in (l for libs in self._libs.values() for l in libs):
            if isinstance(lib, Library):
                propagate_defines(lib, lib.defines)
            yield lib

        for obj in self._binaries.values():
            yield obj

    LIBRARY_NAME_VAR = {
        'host': 'HOST_LIBRARY_NAME',
        'target': 'LIBRARY_NAME',
    }

    def _link_libraries(self, context, obj, variable):
        """Add linkage declarations to a given object."""
        assert isinstance(obj, Linkable)

        for path in context.get(variable, []):
            force_static = path.startswith('static:') and obj.KIND == 'target'
            if force_static:
                path = path[7:]
            name = mozpath.basename(path)
            dir = mozpath.dirname(path)
            candidates = [l for l in self._libs[name] if l.KIND == obj.KIND]
            if dir:
                if dir.startswith('/'):
                    dir = mozpath.normpath(
                        mozpath.join(obj.topobjdir, dir[1:]))
                else:
                    dir = mozpath.normpath(
                        mozpath.join(obj.objdir, dir))
                dir = mozpath.relpath(dir, obj.topobjdir)
                candidates = [l for l in candidates if l.relobjdir == dir]
                if not candidates:
                    # If the given directory is under one of the external
                    # (third party) paths, use a fake library reference to
                    # there.
                    for d in self._external_paths:
                        if dir.startswith('%s/' % d):
                            candidates = [self._get_external_library(dir, name,
                                force_static)]
                            break

                if not candidates:
                    raise SandboxValidationError(
                        '%s contains "%s", but there is no "%s" %s in %s.'
                        % (variable, path, name,
                        self.LIBRARY_NAME_VAR[obj.KIND], dir), context)

            if len(candidates) > 1:
                # If there's more than one remaining candidate, it could be
                # that there are instances for the same library, in static and
                # shared form.
                libs = {}
                for l in candidates:
                    key = mozpath.join(l.relobjdir, l.basename)
                    if force_static:
                        if isinstance(l, StaticLibrary):
                            libs[key] = l
                    else:
                        if key in libs and isinstance(l, SharedLibrary):
                            libs[key] = l
                        if key not in libs:
                            libs[key] = l
                candidates = libs.values()
                if force_static and not candidates:
                    if dir:
                        raise SandboxValidationError(
                            '%s contains "static:%s", but there is no static '
                            '"%s" %s in %s.' % (variable, path, name,
                            self.LIBRARY_NAME_VAR[obj.KIND], dir), context)
                    raise SandboxValidationError(
                        '%s contains "static:%s", but there is no static "%s" '
                        '%s in the tree' % (variable, name, name,
                        self.LIBRARY_NAME_VAR[obj.KIND]), context)

            if not candidates:
                raise SandboxValidationError(
                    '%s contains "%s", which does not match any %s in the tree.'
                    % (variable, path, self.LIBRARY_NAME_VAR[obj.KIND]),
                    context)

            elif len(candidates) > 1:
                paths = (mozpath.join(l.relativedir, 'moz.build')
                    for l in candidates)
                raise SandboxValidationError(
                    '%s contains "%s", which matches a %s defined in multiple '
                    'places:\n    %s' % (variable, path,
                    self.LIBRARY_NAME_VAR[obj.KIND],
                    '\n    '.join(paths)), context)

            elif force_static and not isinstance(candidates[0], StaticLibrary):
                raise SandboxValidationError(
                    '%s contains "static:%s", but there is only a shared "%s" '
                    'in %s. You may want to add FORCE_STATIC_LIB=True in '
                    '%s/moz.build, or remove "static:".' % (variable, path,
                    name, candidates[0].relobjdir, candidates[0].relobjdir),
                    context)

            elif isinstance(obj, StaticLibrary) and isinstance(candidates[0],
                    SharedLibrary):
                self._static_linking_shared.add(obj)
            obj.link_library(candidates[0])

        # Link system libraries from OS_LIBS/HOST_OS_LIBS.
        for lib in context.get(variable.replace('USE', 'OS'), []):
            obj.link_system_library(lib)

    @memoize
    def _get_external_library(self, dir, name, force_static):
        # Create ExternalStaticLibrary or ExternalSharedLibrary object with a
        # context more or less truthful about where the external library is.
        context = Context(config=self.config)
        context.add_source(mozpath.join(self.config.topsrcdir, dir, 'dummy'))
        if force_static:
            return ExternalStaticLibrary(context, name)
        else:
            return ExternalSharedLibrary(context, name)

    def emit_from_context(self, context):
        """Convert a Context to tree metadata objects.

        This is a generator of mozbuild.frontend.data.ContextDerived instances.
        """
        # We always emit a directory traversal descriptor. This is needed by
        # the recursive make backend.
        for o in self._emit_directory_traversal_from_context(context): yield o

        for path in context['CONFIGURE_SUBST_FILES']:
            yield self._create_substitution(ConfigFileSubstitution, context,
                path)

        for path in context['CONFIGURE_DEFINE_FILES']:
            yield self._create_substitution(HeaderFileSubstitution, context,
                path)

        # XPIDL source files get processed and turned into .h and .xpt files.
        # If there are multiple XPIDL files in a directory, they get linked
        # together into a final .xpt, which has the name defined by
        # XPIDL_MODULE.
        xpidl_module = context['XPIDL_MODULE']

        if context['XPIDL_SOURCES'] and not xpidl_module:
            raise SandboxValidationError('XPIDL_MODULE must be defined if '
                'XPIDL_SOURCES is defined.', context)

        if xpidl_module and not context['XPIDL_SOURCES']:
            raise SandboxValidationError('XPIDL_MODULE cannot be defined '
                'unless there are XPIDL_SOURCES', context)

        if context['XPIDL_SOURCES'] and context['NO_DIST_INSTALL']:
            self.log(logging.WARN, 'mozbuild_warning', dict(
                path=context.main_path),
                '{path}: NO_DIST_INSTALL has no effect on XPIDL_SOURCES.')

        for idl in context['XPIDL_SOURCES']:
            yield XPIDLFile(context, mozpath.join(context.srcdir, idl),
                xpidl_module)

        for symbol in ('SOURCES', 'HOST_SOURCES', 'UNIFIED_SOURCES'):
            for src in (context[symbol] or []):
                if not os.path.exists(mozpath.join(context.srcdir, src)):
                    raise SandboxValidationError('File listed in %s does not '
                        'exist: \'%s\'' % (symbol, src), context)

        # Proxy some variables as-is until we have richer classes to represent
        # them. We should aim to keep this set small because it violates the
        # desired abstraction of the build definition away from makefiles.
        passthru = VariablePassthru(context)
        varlist = [
            'ANDROID_GENERATED_RESFILES',
            'ANDROID_RES_DIRS',
            'DISABLE_STL_WRAPPING',
            'EXTRA_ASSEMBLER_FLAGS',
            'EXTRA_COMPILE_FLAGS',
            'EXTRA_COMPONENTS',
            'EXTRA_DSO_LDOPTS',
            'EXTRA_PP_COMPONENTS',
            'FAIL_ON_WARNINGS',
            'FILES_PER_UNIFIED_FILE',
            'USE_STATIC_LIBS',
            'GENERATED_FILES',
            'IS_GYP_DIR',
            'MSVC_ENABLE_PGO',
            'NO_DIST_INSTALL',
            'PYTHON_UNIT_TESTS',
            'RCFILE',
            'RESFILE',
            'RCINCLUDE',
            'DEFFILE',
            'WIN32_EXE_LDFLAGS',
            'LD_VERSION_SCRIPT',
        ]
        for v in varlist:
            if v in context and context[v]:
                passthru.variables[v] = context[v]

        for v in ['CFLAGS', 'CXXFLAGS', 'CMFLAGS', 'CMMFLAGS', 'LDFLAGS']:
            if v in context and context[v]:
                passthru.variables['MOZBUILD_' + v] = context[v]

        # NO_VISIBILITY_FLAGS is slightly different
        if context['NO_VISIBILITY_FLAGS']:
            passthru.variables['VISIBILITY_FLAGS'] = ''

        if context['DELAYLOAD_DLLS']:
            passthru.variables['DELAYLOAD_LDFLAGS'] = [('-DELAYLOAD:%s' % dll)
                for dll in context['DELAYLOAD_DLLS']]
            passthru.variables['USE_DELAYIMP'] = True

        varmap = dict(
            SOURCES={
                '.s': 'ASFILES',
                '.asm': 'ASFILES',
                '.c': 'CSRCS',
                '.m': 'CMSRCS',
                '.mm': 'CMMSRCS',
                '.cc': 'CPPSRCS',
                '.cpp': 'CPPSRCS',
                '.cxx': 'CPPSRCS',
                '.S': 'SSRCS',
            },
            HOST_SOURCES={
                '.c': 'HOST_CSRCS',
                '.mm': 'HOST_CMMSRCS',
                '.cc': 'HOST_CPPSRCS',
                '.cpp': 'HOST_CPPSRCS',
                '.cxx': 'HOST_CPPSRCS',
            },
            UNIFIED_SOURCES={
                '.c': 'UNIFIED_CSRCS',
                '.mm': 'UNIFIED_CMMSRCS',
                '.cc': 'UNIFIED_CPPSRCS',
                '.cpp': 'UNIFIED_CPPSRCS',
                '.cxx': 'UNIFIED_CPPSRCS',
            }
        )
        varmap.update(dict(('GENERATED_%s' % k, v) for k, v in varmap.items()
                           if k in ('SOURCES', 'UNIFIED_SOURCES')))
        for variable, mapping in varmap.items():
            for f in context[variable]:
                ext = mozpath.splitext(f)[1]
                if ext not in mapping:
                    raise SandboxValidationError(
                        '%s has an unknown file type.' % f, context)
                l = passthru.variables.setdefault(mapping[ext], [])
                l.append(f)
                if variable.startswith('GENERATED_'):
                    l = passthru.variables.setdefault('GARBAGE', [])
                    l.append(f)

        no_pgo = context.get('NO_PGO')
        sources = context.get('SOURCES', [])
        no_pgo_sources = [f for f in sources if sources[f].no_pgo]
        if no_pgo:
            if no_pgo_sources:
                raise SandboxValidationError('NO_PGO and SOURCES[...].no_pgo '
                    'cannot be set at the same time', context)
            passthru.variables['NO_PROFILE_GUIDED_OPTIMIZE'] = no_pgo
        if no_pgo_sources:
            passthru.variables['NO_PROFILE_GUIDED_OPTIMIZE'] = no_pgo_sources

        sources_with_flags = [f for f in sources if sources[f].flags]
        for f in sources_with_flags:
            ext = mozpath.splitext(f)[1]
            yield PerSourceFlag(context, f, sources[f].flags)

        exports = context.get('EXPORTS')
        if exports:
            yield Exports(context, exports,
                dist_install=not context.get('NO_DIST_INSTALL', False))

        test_harness_files = context.get('TEST_HARNESS_FILES')
        if test_harness_files:
            srcdir_files = defaultdict(list)
            srcdir_pattern_files = defaultdict(list)
            objdir_files = defaultdict(list)

            for path, strings in test_harness_files.walk():
                if not path and strings:
                    raise SandboxValidationError(
                        'Cannot install files to the root of TEST_HARNESS_FILES', context)

                for s in strings:
                    if context.is_objdir_path(s):
                        if s.startswith('!/'):
                            raise SandboxValidationError(
                                'Topobjdir-relative file not allowed in TEST_HARNESS_FILES: %s' % s, context)
                        objdir_files[path].append(s[1:])
                    else:
                        resolved = context.resolve_path(s)
                        if '*' in s:
                            srcdir_pattern_files[path].append(s);
                        elif not os.path.exists(resolved):
                            raise SandboxValidationError(
                                'File listed in TEST_HARNESS_FILES does not exist: %s' % s, context)
                        else:
                            srcdir_files[path].append(resolved)

            yield TestHarnessFiles(context, srcdir_files,
                                   srcdir_pattern_files, objdir_files)

        defines = context.get('DEFINES')
        if defines:
            yield Defines(context, defines)

        resources = context.get('RESOURCE_FILES')
        if resources:
            yield Resources(context, resources, defines)

        for kind, cls in [('PROGRAM', Program), ('HOST_PROGRAM', HostProgram)]:
            program = context.get(kind)
            if program:
                if program in self._binaries:
                    raise SandboxValidationError(
                        'Cannot use "%s" as %s name, '
                        'because it is already used in %s' % (program, kind,
                        self._binaries[program].relativedir), context)
                self._binaries[program] = cls(context, program)
                self._linkage.append((context, self._binaries[program],
                    kind.replace('PROGRAM', 'USE_LIBS')))

        for kind, cls in [
                ('SIMPLE_PROGRAMS', SimpleProgram),
                ('CPP_UNIT_TESTS', SimpleProgram),
                ('HOST_SIMPLE_PROGRAMS', HostSimpleProgram)]:
            for program in context[kind]:
                if program in self._binaries:
                    raise SandboxValidationError(
                        'Cannot use "%s" in %s, '
                        'because it is already used in %s' % (program, kind,
                        self._binaries[program].relativedir), context)
                self._binaries[program] = cls(context, program,
                    is_unit_test=kind == 'CPP_UNIT_TESTS')
                self._linkage.append((context, self._binaries[program],
                    'HOST_USE_LIBS' if kind == 'HOST_SIMPLE_PROGRAMS'
                    else 'USE_LIBS'))

        extra_js_modules = context.get('EXTRA_JS_MODULES')
        if extra_js_modules:
            yield JavaScriptModules(context, extra_js_modules, 'extra')

        extra_pp_js_modules = context.get('EXTRA_PP_JS_MODULES')
        if extra_pp_js_modules:
            yield JavaScriptModules(context, extra_pp_js_modules, 'extra_pp')

        test_js_modules = context.get('TESTING_JS_MODULES')
        if test_js_modules:
            yield JavaScriptModules(context, test_js_modules, 'testing')

        simple_lists = [
            ('GENERATED_EVENTS_WEBIDL_FILES', GeneratedEventWebIDLFile),
            ('GENERATED_WEBIDL_FILES', GeneratedWebIDLFile),
            ('IPDL_SOURCES', IPDLFile),
            ('GENERATED_INCLUDES', GeneratedInclude),
            ('PREPROCESSED_TEST_WEBIDL_FILES', PreprocessedTestWebIDLFile),
            ('PREPROCESSED_WEBIDL_FILES', PreprocessedWebIDLFile),
            ('TEST_WEBIDL_FILES', TestWebIDLFile),
            ('WEBIDL_FILES', WebIDLFile),
            ('WEBIDL_EXAMPLE_INTERFACES', ExampleWebIDLInterface),
        ]
        for context_var, klass in simple_lists:
            for name in context.get(context_var, []):
                yield klass(context, name)

        for local_include in context.get('LOCAL_INCLUDES', []):
            if local_include.startswith('/'):
                path = context.config.topsrcdir
                relative_include = local_include[1:]
            else:
                path = context.srcdir
                relative_include = local_include

            actual_include = os.path.join(path, relative_include)
            if not os.path.exists(actual_include):
                raise SandboxValidationError('Path specified in LOCAL_INCLUDES '
                    'does not exist: %s (resolved to %s)' % (local_include, actual_include), context)
            yield LocalInclude(context, local_include)

        if context.get('FINAL_TARGET') or context.get('XPI_NAME') or \
                context.get('DIST_SUBDIR'):
            yield InstallationTarget(context)

        host_libname = context.get('HOST_LIBRARY_NAME')
        libname = context.get('LIBRARY_NAME')

        if host_libname:
            if host_libname == libname:
                raise SandboxValidationError('LIBRARY_NAME and '
                    'HOST_LIBRARY_NAME must have a different value', context)
            lib = HostLibrary(context, host_libname)
            self._libs[host_libname].append(lib)
            self._linkage.append((context, lib, 'HOST_USE_LIBS'))

        final_lib = context.get('FINAL_LIBRARY')
        if not libname and final_lib:
            # If no LIBRARY_NAME is given, create one.
            libname = context.relsrcdir.replace('/', '_')

        static_lib = context.get('FORCE_STATIC_LIB')
        shared_lib = context.get('FORCE_SHARED_LIB')

        static_name = context.get('STATIC_LIBRARY_NAME')
        shared_name = context.get('SHARED_LIBRARY_NAME')

        is_framework = context.get('IS_FRAMEWORK')
        is_component = context.get('IS_COMPONENT')

        soname = context.get('SONAME')

        lib_defines = context.get('LIBRARY_DEFINES')

        shared_args = {}
        static_args = {}

        if final_lib:
            if static_lib:
                raise SandboxValidationError(
                    'FINAL_LIBRARY implies FORCE_STATIC_LIB. '
                    'Please remove the latter.', context)
            if shared_lib:
                raise SandboxValidationError(
                    'FINAL_LIBRARY conflicts with FORCE_SHARED_LIB. '
                    'Please remove one.', context)
            if is_framework:
                raise SandboxValidationError(
                    'FINAL_LIBRARY conflicts with IS_FRAMEWORK. '
                    'Please remove one.', context)
            if is_component:
                raise SandboxValidationError(
                    'FINAL_LIBRARY conflicts with IS_COMPONENT. '
                    'Please remove one.', context)
            static_args['link_into'] = final_lib
            static_lib = True

        if libname:
            if is_component:
                if static_lib:
                    raise SandboxValidationError(
                        'IS_COMPONENT conflicts with FORCE_STATIC_LIB. '
                        'Please remove one.', context)
                shared_lib = True
                shared_args['variant'] = SharedLibrary.COMPONENT

            if is_framework:
                if soname:
                    raise SandboxValidationError(
                        'IS_FRAMEWORK conflicts with SONAME. '
                        'Please remove one.', context)
                shared_lib = True
                shared_args['variant'] = SharedLibrary.FRAMEWORK

            if static_name:
                if not static_lib:
                    raise SandboxValidationError(
                        'STATIC_LIBRARY_NAME requires FORCE_STATIC_LIB',
                        context)
                static_args['real_name'] = static_name

            if shared_name:
                if not shared_lib:
                    raise SandboxValidationError(
                        'SHARED_LIBRARY_NAME requires FORCE_SHARED_LIB',
                        context)
                shared_args['real_name'] = shared_name

            if soname:
                if not shared_lib:
                    raise SandboxValidationError(
                        'SONAME requires FORCE_SHARED_LIB', context)
                shared_args['soname'] = soname

            if not static_lib and not shared_lib:
                static_lib = True

            # If both a shared and a static library are created, only the
            # shared library is meant to be a SDK library.
            if context.get('SDK_LIBRARY'):
                if shared_lib:
                    shared_args['is_sdk'] = True
                elif static_lib:
                    static_args['is_sdk'] = True

            if shared_lib and static_lib:
                if not static_name and not shared_name:
                    raise SandboxValidationError(
                        'Both FORCE_STATIC_LIB and FORCE_SHARED_LIB are True, '
                        'but neither STATIC_LIBRARY_NAME or '
                        'SHARED_LIBRARY_NAME is set. At least one is required.',
                        context)
                if static_name and not shared_name and static_name == libname:
                    raise SandboxValidationError(
                        'Both FORCE_STATIC_LIB and FORCE_SHARED_LIB are True, '
                        'but STATIC_LIBRARY_NAME is the same as LIBRARY_NAME, '
                        'and SHARED_LIBRARY_NAME is unset. Please either '
                        'change STATIC_LIBRARY_NAME or LIBRARY_NAME, or set '
                        'SHARED_LIBRARY_NAME.', context)
                if shared_name and not static_name and shared_name == libname:
                    raise SandboxValidationError(
                        'Both FORCE_STATIC_LIB and FORCE_SHARED_LIB are True, '
                        'but SHARED_LIBRARY_NAME is the same as LIBRARY_NAME, '
                        'and STATIC_LIBRARY_NAME is unset. Please either '
                        'change SHARED_LIBRARY_NAME or LIBRARY_NAME, or set '
                        'STATIC_LIBRARY_NAME.', context)
                if shared_name and static_name and shared_name == static_name:
                    raise SandboxValidationError(
                        'Both FORCE_STATIC_LIB and FORCE_SHARED_LIB are True, '
                        'but SHARED_LIBRARY_NAME is the same as '
                        'STATIC_LIBRARY_NAME. Please change one of them.',
                        context)

            if shared_lib:
                lib = SharedLibrary(context, libname, **shared_args)
                self._libs[libname].append(lib)
                self._linkage.append((context, lib, 'USE_LIBS'))
            if static_lib:
                lib = StaticLibrary(context, libname, **static_args)
                self._libs[libname].append(lib)
                self._linkage.append((context, lib, 'USE_LIBS'))

            if lib_defines:
                if not libname:
                    raise SandboxValidationError('LIBRARY_DEFINES needs a '
                        'LIBRARY_NAME to take effect', context)
                lib.defines.update(lib_defines)

        # While there are multiple test manifests, the behavior is very similar
        # across them. We enforce this by having common handling of all
        # manifests and outputting a single class type with the differences
        # described inside the instance.
        #
        # Keys are variable prefixes and values are tuples describing how these
        # manifests should be handled:
        #
        #    (flavor, install_prefix, active)
        #
        # flavor identifies the flavor of this test.
        # install_prefix is the path prefix of where to install the files in
        #     the tests directory.
        # active indicates whether to filter out inactive tests from the
        #     manifest.
        #
        # We ideally don't filter out inactive tests. However, not every test
        # harness can yet deal with test filtering. Once all harnesses can do
        # this, this feature can be dropped.
        test_manifests = dict(
            A11Y=('a11y', 'testing/mochitest', 'a11y', True),
            BROWSER_CHROME=('browser-chrome', 'testing/mochitest', 'browser', True),
            METRO_CHROME=('metro-chrome', 'testing/mochitest', 'metro', True),
            MOCHITEST=('mochitest', 'testing/mochitest', 'tests', True),
            MOCHITEST_CHROME=('chrome', 'testing/mochitest', 'chrome', True),
            MOCHITEST_WEBAPPRT_CHROME=('webapprt-chrome', 'testing/mochitest', 'webapprtChrome', True),
            WEBRTC_SIGNALLING_TEST=('steeplechase', 'steeplechase', '.', True),
            XPCSHELL_TESTS=('xpcshell', 'xpcshell', '.', False),
        )

        for prefix, info in test_manifests.items():
            for path in context.get('%s_MANIFESTS' % prefix, []):
                for obj in self._process_test_manifest(context, info, path):
                    yield obj

        for flavor in ('crashtest', 'reftest'):
            for path in context.get('%s_MANIFESTS' % flavor.upper(), []):
                for obj in self._process_reftest_manifest(context, flavor, path):
                    yield obj

        jar_manifests = context.get('JAR_MANIFESTS', [])
        if len(jar_manifests) > 1:
            raise SandboxValidationError('While JAR_MANIFESTS is a list, '
                'it is currently limited to one value.', context)

        for path in jar_manifests:
            yield JARManifest(context, mozpath.join(context.srcdir, path))

        # Temporary test to look for jar.mn files that creep in without using
        # the new declaration. Before, we didn't require jar.mn files to
        # declared anywhere (they were discovered). This will detect people
        # relying on the old behavior.
        if os.path.exists(os.path.join(context.srcdir, 'jar.mn')):
            if 'jar.mn' not in jar_manifests:
                raise SandboxValidationError('A jar.mn exists but it '
                    'is not referenced in the moz.build file. '
                    'Please define JAR_MANIFESTS.', context)

        for name, jar in context.get('JAVA_JAR_TARGETS', {}).items():
            yield ContextWrapped(context, jar)

        for name, data in context.get('ANDROID_ECLIPSE_PROJECT_TARGETS', {}).items():
            yield ContextWrapped(context, data)

        if passthru.variables:
            yield passthru

    def _create_substitution(self, cls, context, path):
        if os.path.isabs(path):
            path = path[1:]

        sub = cls(context)
        sub.input_path = mozpath.join(context.srcdir, '%s.in' % path)
        sub.output_path = mozpath.join(context.objdir, path)
        sub.relpath = path

        return sub

    def _process_test_manifest(self, context, info, manifest_path):
        flavor, install_root, install_subdir, filter_inactive = info

        manifest_path = mozpath.normpath(manifest_path)
        path = mozpath.normpath(mozpath.join(context.srcdir, manifest_path))
        manifest_dir = mozpath.dirname(path)
        manifest_reldir = mozpath.dirname(mozpath.relpath(path,
            context.config.topsrcdir))
        install_prefix = mozpath.join(install_root, install_subdir)

        try:
            m = manifestparser.TestManifest(manifests=[path], strict=True)
            defaults = m.manifest_defaults[os.path.normpath(path)]
            if not m.tests and not 'support-files' in defaults:
                raise SandboxValidationError('Empty test manifest: %s'
                    % path, context)

            obj = TestManifest(context, path, m, flavor=flavor,
                install_prefix=install_prefix,
                relpath=mozpath.join(manifest_reldir, mozpath.basename(path)),
                dupe_manifest='dupe-manifest' in defaults)

            filtered = m.tests

            if filter_inactive:
                # We return tests that don't exist because we want manifests
                # defining tests that don't exist to result in error.
                filtered = m.active_tests(exists=False, disabled=True,
                    **self.info)

                missing = [t['name'] for t in filtered if not os.path.exists(t['path'])]
                if missing:
                    raise SandboxValidationError('Test manifest (%s) lists '
                        'test that does not exist: %s' % (
                        path, ', '.join(missing)), context)

            out_dir = mozpath.join(install_prefix, manifest_reldir)
            if 'install-to-subdir' in defaults:
                # This is terrible, but what are you going to do?
                out_dir = mozpath.join(out_dir, defaults['install-to-subdir'])
                obj.manifest_obj_relpath = mozpath.join(manifest_reldir,
                                                        defaults['install-to-subdir'],
                                                        mozpath.basename(path))


            # "head" and "tail" lists.
            # All manifests support support-files.
            #
            # Keep a set of already seen support file patterns, because
            # repeatedly processing the patterns from the default section
            # for every test is quite costly (see bug 922517).
            extras = (('head', set()),
                      ('tail', set()),
                      ('support-files', set()))
            def process_support_files(test):
                for thing, seen in extras:
                    value = test.get(thing, '')
                    if value in seen:
                        continue
                    seen.add(value)
                    for pattern in value.split():
                        # We only support globbing on support-files because
                        # the harness doesn't support * for head and tail.
                        if '*' in pattern and thing == 'support-files':
                            obj.pattern_installs.append(
                                (manifest_dir, pattern, out_dir))
                        # "absolute" paths identify files that are to be
                        # placed in the install_root directory (no globs)
                        elif pattern[0] == '/':
                            full = mozpath.normpath(mozpath.join(manifest_dir,
                                mozpath.basename(pattern)))
                            obj.installs[full] = (mozpath.join(install_root,
                                pattern[1:]), False)
                        else:
                            full = mozpath.normpath(mozpath.join(manifest_dir,
                                pattern))

                            dest_path = mozpath.join(out_dir, pattern)

                            # If the path resolves to a different directory
                            # tree, we take special behavior depending on the
                            # entry type.
                            if not full.startswith(manifest_dir):
                                # If it's a support file, we install the file
                                # into the current destination directory.
                                # This implementation makes installing things
                                # with custom prefixes impossible. If this is
                                # needed, we can add support for that via a
                                # special syntax later.
                                if thing == 'support-files':
                                    dest_path = mozpath.join(out_dir,
                                        os.path.basename(pattern))
                                # If it's not a support file, we ignore it.
                                # This preserves old behavior so things like
                                # head files doesn't get installed multiple
                                # times.
                                else:
                                    continue

                            obj.installs[full] = (mozpath.normpath(dest_path),
                                False)

            for test in filtered:
                obj.tests.append(test)

                obj.installs[mozpath.normpath(test['path'])] = \
                    (mozpath.join(out_dir, test['relpath']), True)

                process_support_files(test)

            if not filtered:
                # If there are no tests, look for support-files under DEFAULT.
                process_support_files(defaults)

            # We also copy manifests into the output directory,
            # including manifests from [include:foo] directives.
            for mpath in m.manifests():
                mpath = mozpath.normpath(mpath)
                out_path = mozpath.join(out_dir, mozpath.basename(mpath))
                obj.installs[mpath] = (out_path, False)

            # Some manifests reference files that are auto generated as
            # part of the build or shouldn't be installed for some
            # reason. Here, we prune those files from the install set.
            # FUTURE we should be able to detect autogenerated files from
            # other build metadata. Once we do that, we can get rid of this.
            for f in defaults.get('generated-files', '').split():
                # We re-raise otherwise the stack trace isn't informative.
                try:
                    del obj.installs[mozpath.join(manifest_dir, f)]
                except KeyError:
                    raise SandboxValidationError('Error processing test '
                        'manifest %s: entry in generated-files not present '
                        'elsewhere in manifest: %s' % (path, f), context)

                obj.external_installs.add(mozpath.join(out_dir, f))

            yield obj
        except (AssertionError, Exception):
            raise SandboxValidationError('Error processing test '
                'manifest file %s: %s' % (path,
                    '\n'.join(traceback.format_exception(*sys.exc_info()))),
                context)

    def _process_reftest_manifest(self, context, flavor, manifest_path):
        manifest_path = mozpath.normpath(manifest_path)
        manifest_full_path = mozpath.normpath(mozpath.join(
            context.srcdir, manifest_path))
        manifest_reldir = mozpath.dirname(mozpath.relpath(manifest_full_path,
            context.config.topsrcdir))

        manifest = reftest.ReftestManifest()
        manifest.load(manifest_full_path)

        # reftest manifests don't come from manifest parser. But they are
        # similar enough that we can use the same emitted objects. Note
        # that we don't perform any installs for reftests.
        obj = TestManifest(context, manifest_full_path, manifest,
                flavor=flavor, install_prefix='%s/' % flavor,
                relpath=mozpath.join(manifest_reldir,
                    mozpath.basename(manifest_path)))

        for test in sorted(manifest.files):
            obj.tests.append({
                'path': test,
                'here': mozpath.dirname(test),
                'manifest': manifest_full_path,
                'name': mozpath.basename(test),
                'head': '',
                'tail': '',
                'support-files': '',
                'subsuite': '',
            })

        yield obj

    def _emit_directory_traversal_from_context(self, context):
        o = DirectoryTraversal(context)
        o.dirs = context.get('DIRS', [])
        o.test_dirs = context.get('TEST_DIRS', [])
        o.affected_tiers = context.get_affected_tiers()

        # Some paths have a subconfigure, yet also have a moz.build. Those
        # shouldn't end up in self._external_paths.
        self._external_paths -= { o.relobjdir }

        if 'TIERS' in context:
            for tier in context['TIERS']:
                o.tier_dirs[tier] = context['TIERS'][tier]['regular'] + \
                    context['TIERS'][tier]['external']

        yield o
