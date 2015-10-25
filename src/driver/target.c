/*
 * This file is part of cparser.
 * Copyright (C) 2014 Matthias Braun <matze@braunis.de>
 */
#include <libfirm/be.h>
#include <libfirm/firm.h>

#include "adt/panic.h"
#include "adt/strutil.h"
#include "adt/util.h"
#include "ast/type_t.h"
#include "ast/types.h"
#include "c_driver.h"
#include "diagnostic.h"
#include "firm/ast2firm.h"
#include "firm/mangle.h"
#include "lang_features.h"
#include "target.h"
#include "warning.h"

target_t target = {
	.pic_mode = -1,
};
const char *multilib_directory_target_triple;
static const char *experimental_backend;

/**
 * initialize cparser type properties based on a firm type
 */
static void set_typeprops_type(atomic_type_properties_t* props, ir_type *type)
{
	props->size             = get_type_size_bytes(type);
	props->alignment        = get_type_alignment_bytes(type);
	props->struct_alignment = props->alignment;
}

/**
 * Copy atomic type properties except the integer conversion rank
 */
static void copy_typeprops(atomic_type_properties_t *dest,
                           const atomic_type_properties_t *src)
{
	dest->size             = src->size;
	dest->alignment        = src->alignment;
	dest->struct_alignment = src->struct_alignment;
	dest->flags            = src->flags;
}

void target_adjust_types_and_dialect(void)
{
	const backend_params *be_params = be_get_backend_param();
	unsigned machine_size = be_params->machine_size;
	if (machine_size % BITS_PER_BYTE != 0)
		panic("Invalid target machine_size");
	unsigned pointer_size = machine_size / BITS_PER_BYTE;
	unsigned int_size     = MIN(pointer_size, 4);
	unsigned long_size    = MIN(pointer_size, 8);
	/* to ease porting of old c-code microsoft decided to use 32bits
	 * even for long */
	const char *operating_system = target.machine->operating_system;
	if (is_windows_os(operating_system) && pointer_size == 8)
		long_size = 4;
	init_types(int_size, long_size, pointer_size);

	dialect.pointer_sized_int  = ATOMIC_TYPE_LONG;
	dialect.pointer_sized_uint = ATOMIC_TYPE_ULONG;

	atomic_type_properties_t *props = atomic_type_properties;

	/* adjust types as requested by target architecture */
	ir_type *const type_ld = be_params->type_long_double;
	if (type_ld)
		set_typeprops_type(&props[ATOMIC_TYPE_LONG_DOUBLE], type_ld);

	ir_type *const type_ll = be_params->type_long_long;
	if (type_ll)
		set_typeprops_type(&props[ATOMIC_TYPE_LONGLONG], type_ll);

	ir_type *const type_ull = be_params->type_unsigned_long_long;
	if (type_ull)
		set_typeprops_type(&props[ATOMIC_TYPE_ULONGLONG], type_ull);

	/* operating system ABI specifics */
	if (is_darwin_os(operating_system)) {
		if (streq(firm_isa, "ia32")) {
			props[ATOMIC_TYPE_LONGLONG].struct_alignment    =  4;
			props[ATOMIC_TYPE_ULONGLONG].struct_alignment   =  4;
			props[ATOMIC_TYPE_DOUBLE].struct_alignment      =  4;
		}
		if (streq(firm_isa, "ia32") || streq(firm_isa, "amd64")) {
			props[ATOMIC_TYPE_LONG_DOUBLE].size             = 16;
			props[ATOMIC_TYPE_LONG_DOUBLE].alignment        = 16;
			props[ATOMIC_TYPE_LONG_DOUBLE].struct_alignment = 16;
		}
	} else if (is_windows_os(operating_system)) {
		props[ATOMIC_TYPE_LONGLONG].struct_alignment  = 8;
		props[ATOMIC_TYPE_ULONGLONG].struct_alignment = 8;
		props[ATOMIC_TYPE_DOUBLE].struct_alignment    = 8;
		props[ATOMIC_TYPE_LONG_DOUBLE] = props[ATOMIC_TYPE_DOUBLE];
		if (machine_size == 32) {
			dialect.pointer_sized_int  = ATOMIC_TYPE_INT;
			dialect.pointer_sized_uint = ATOMIC_TYPE_UINT;
		} else {
			dialect.pointer_sized_int  = ATOMIC_TYPE_LONGLONG;
			dialect.pointer_sized_uint = ATOMIC_TYPE_ULONGLONG;
		}
	} else if (streq(firm_isa, "ia32")) {
		props[ATOMIC_TYPE_DOUBLE].struct_alignment    = 4;
		props[ATOMIC_TYPE_LONGLONG].struct_alignment  = 4;
		props[ATOMIC_TYPE_ULONGLONG].struct_alignment = 4;
	}

	/* stuff decided after processing operating system specifics and
	 * commandline flags */
	if (dialect.char_is_signed) {
		props[ATOMIC_TYPE_CHAR].flags |= ATOMIC_TYPE_FLAG_SIGNED;
	} else {
		props[ATOMIC_TYPE_CHAR].flags &= ~ATOMIC_TYPE_FLAG_SIGNED;
	}
	/* copy over wchar_t properties (including rank) */
	props[ATOMIC_TYPE_WCHAR_T] = props[dialect.wchar_atomic_kind];

	/* initialize defaults for unsupported types */
	if (!type_ld)
		copy_typeprops(&props[ATOMIC_TYPE_LONG_DOUBLE],
		               &props[ATOMIC_TYPE_DOUBLE]);
	static bool had_cpp_warning;
	if (dialect.cpp && !had_cpp_warning) {
		warningf(WARN_EXPERIMENTAL, NULL,
		         "C++ support is highly experimental and unfinished");
		had_cpp_warning = true;
	}
}

static ident *compilerlib_name_mangle_default(ident *id, ir_type *mt)
{
	(void)mt;
	return id;
}

static ident *compilerlib_name_mangle_underscore(ident *id, ir_type *mt)
{
	(void)mt;
	return new_id_fmt("_%s", id);
}

static void set_be_option(const char *arg)
{
	int res = be_parse_arg(arg);
	if (!res)
		panic("setting firm backend option failed");
}

/** Add a target specific preprocessor define. */
static target_define_t *ppdef(const char *name, const char *value)
{
	target_define_t *define = calloc(1, sizeof(*define));
	define->name = name;
	define->value = value;
	define->next = target.defines;
	target.defines = define;
	return define;
}

/** Add a target specific preprocessor define. This calls \p condition_func
 * to check whether the define should really be added. */
static target_define_t *ppdefc(const char *name, const char *value,
                               bool (*condition_func)(void))
{
	target_define_t *define = ppdef(name, value);
	define->condition = condition_func;
	return define;
}

static bool cond_not_strict(void)
{
	return dialect.gnu;
}

static bool cond_is_little_endian(void)
{
	return !target.byte_order_big_endian;
}

static void init_os_support(void)
{
	/* Note: Code here should only check the target triple! Querying other
	 * target features is not allowed as subsequent commandline options may
	 * change those. Example:
	 * ppdefc("X", "Y", cond_not_strict); // Correct: cond_not_strict is
	 *                                    // evaluated later
	 * if (dialect.gnu)
	 *    ppdef("X", "Y"); // Wrong: language dialect/target is not final yet
	 */

	const char *os  = target.machine->operating_system;
	const char *cpu = target.machine->cpu_type;
	target.enable_main_collect2_hack = false;
	target.biggest_alignment = 16;
	dialect.wchar_atomic_kind = ATOMIC_TYPE_INT;

	if (is_elf_os(os)) {
		driver_default_exe_output = "a.out";
		set_create_ld_ident(create_name_linux_elf);
		target.user_label_prefix = "";
		target.object_format = OBJECT_FORMAT_ELF;
		set_be_option("ia32-struct_in_reg=no");
		set_be_option("amd64-x64abi=no");
		set_compilerlib_name_mangle(compilerlib_name_mangle_default);
		ppdef( "__unix",    "1");
		ppdef( "__unix__",  "1");
		ppdefc("unix",      "1", cond_not_strict);
		ppdef( "__linux",   "1");
		ppdef( "__linux__", "1");
		ppdefc("linux",     "1", cond_not_strict);
		if (strstr(os, "gnu") != NULL)
			ppdef("__gnu_linux__", "1");
	} else if (is_darwin_os(os)) {
		driver_default_exe_output = "a.out";
		set_create_ld_ident(create_name_macho);
		target.user_label_prefix = "_";
		target.object_format = OBJECT_FORMAT_MACH_O;
		set_be_option("ia32-stackalign=4");
		set_be_option("ia32-struct_in_reg=yes");
		set_be_option("amd64-x64abi=no");
		set_compilerlib_name_mangle(compilerlib_name_mangle_underscore);
		if (target.pic_mode == -1)
			target.pic_mode = 2;
		ppdef( "__MACH__",               "1");
		ppdef( "__APPLE__",              "1");
		ppdef( "__APPLE_CC__",           "1");
		ppdef( "__weak",                 "");
		ppdef( "__strong",               "");
		ppdef( "__CONSTANT_CFSTRINGS__", "1");
		ppdef( "__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__", "1050");
		ppdef( "__DYNAMIC__",            "1");
		ppdefc("__LITTLE_ENDIAN__",      "1", cond_is_little_endian);
	} else if (is_windows_os(os)) {
		dialect.wchar_atomic_kind = ATOMIC_TYPE_USHORT;
		driver_default_exe_output = "a.exe";
		target.object_format = OBJECT_FORMAT_PE_COFF;
		set_be_option("ia32-struct_in_reg=no");
		bool is_mingw = strstr(os, "mingw") != NULL;
		if (is_mingw) {
			target.enable_main_collect2_hack = true;
			ppdef("__MINGW32__", "1");
		}
		ppdef( "__MSVCRT__", "1");
		ppdef( "_WINNT",     "1");
		ppdef( "__WINNT",    "1");
		ppdef( "__WINNT__",  "1");
		ppdefc("WINNT",      "1", cond_not_strict);
		ppdef( "_WIN32",     "1");
		ppdef( "__WIN32",    "1");
		ppdef( "__WIN32__",  "1");
		ppdefc("WIN32",      "1", cond_not_strict);
		if (streq(cpu, "x86_64")) {
			set_be_option("amd64-x64abi=yes");
			set_create_ld_ident(create_name_win64);
			target.user_label_prefix = "";
			set_compilerlib_name_mangle(compilerlib_name_mangle_default);
			ppdef( "_WIN64",    "1");
			ppdef( "__WIN64",   "1");
			ppdef( "__WIN64__", "1");
			ppdefc("WIN64",     "1", cond_not_strict);
			if (is_mingw)
				ppdef("__MINGW64__", "1");
		} else {
			set_create_ld_ident(create_name_win32);
			target.user_label_prefix = "_";
			set_compilerlib_name_mangle(compilerlib_name_mangle_underscore);
		}
	} else {
		errorf(NULL, "unknown operating system '%s' in target-triple", os);
		exit(EXIT_FAILURE);
	}

	const char *manufacturer = target.machine->manufacturer;
	if (is_ia32_cpu(cpu)) {
		ppdefc("i386",     "1", cond_not_strict);
		ppdef( "__i386",   "1");
		ppdef( "__i386__", "1");
		if (streq(cpu, "i486")) {
			ppdef("__i486",   "1");
			ppdef("__i486__", "1");
		} else if (streq(cpu, "i586")) {
			ppdef("__i586",      "1");
			ppdef("__i586__",    "1");
			ppdef("__pentium",   "1");
			ppdef("__pentium__", "1");
			//ppdef("__pentium_mmx__", "1");
		} else if (streq(cpu, "i686")) {
			ppdef("__pentiumpro",   "1");
			ppdef("__pentiumpro__", "1");
			ppdef("__i686",         "1");
			ppdef("__i686__",       "1");
		} else if (streq(cpu, "i786")) {
			ppdef("__pentium4",     "1");
			ppdef("__pentium4__",   "1");
		}
	} else if (streq(cpu, "sparc")) {
		ppdefc("sparc",     "1", cond_not_strict);
		ppdef( "__sparc",   "1");
		ppdef( "__sparc__", "1");
		/* we always produce sparc V8 code at the moment */
		ppdef( "__sparc_v8__", "1");
		if (strstr(manufacturer, "leon") != NULL)
			ppdef("__leon__", "1");
	} else if (streq(cpu, "arm")) {
		/* TODO: test, what about
		 * ARM_FEATURE_UNALIGNED, ARMEL, ARM_ARCH_7A, ARM_FEATURE_DSP, ... */
		ppdef("__arm__",   "1");
		if (strstr(os, "eabi") != NULL)
			ppdef("__ARM_EABI__", "1");
	} else if (streq(cpu, "x86_64")) {
		ppdef("__x86_64",   "1");
		ppdef("__x86_64__", "1");
		ppdef("__amd64",    "1");
		ppdef("__amd64__",  "1");
	}

	if (target.pic_mode == -1)
		target.pic_mode = 0;
}

static unsigned get_bitsize_codegen_opt(void)
{
	unsigned size = 0;
	for (codegen_option_t *option = codegen_options; option != NULL;
	     option = option->next) {
		const char *opt = option->option;
		if (opt[0] < '0' || opt[0] > '9')
			continue;
		size = atoi(opt);
	}
	return size;
}

static void setup_isa(const char *isa)
{
	char buf[64];
	snprintf(buf, sizeof(buf), "isa=%s", isa);
	set_be_option(buf);

	if (firm_isa != isa)
		strcpy(firm_isa, isa);
}

static void setup_x86_64(void)
{
	experimental_backend
		= "the x86_64 backend is highly experimental and unfinished (consider the -m32 switch)";
	setup_isa("amd64");
}

static void setup_arm(void)
{
	experimental_backend
		= "the arm backend is highly experimental and unfinished";
	setup_isa("arm");
}

void warn_experimental_target(void)
{
	if (experimental_backend != NULL)
		warningf(WARN_EXPERIMENTAL, NULL, "%s", experimental_backend);
}

static void setup_ia32(const char *firm_arch)
{
	set_be_option("isa=ia32");
	char buf[64];
	snprintf(buf, sizeof(buf), "ia32-arch=%s", firm_arch);
	set_be_option(buf);

	strcpy(firm_isa, "ia32");
}

static bool setup_firm_isa(void)
{
	if (firm_isa[0] != '\0') {
		setup_isa(firm_isa);
		return true;
	}

	const char *cpu = target.machine->cpu_type;
	if (streq(cpu, "i386") || streq(cpu, "i486")
	 || streq(cpu, "i586") || streq(cpu, "i686")) {
		if (get_bitsize_codegen_opt() == 64) {
			free(target.machine->cpu_type);
			target.machine->cpu_type = xstrdup("x86_64");
			setup_x86_64();
		} else {
			setup_ia32(cpu);
		}
	} else if (streq(cpu, "x86_64")) {
		if (get_bitsize_codegen_opt() == 32) {
			free(target.machine->cpu_type);
			target.machine->cpu_type = xstrdup("i686");
			setup_ia32("i686");
		} else {
			setup_x86_64();
		}
	} else if (streq(cpu, "sparc")) {
		setup_isa("sparc");
		const char *manufacturer = target.machine->manufacturer;
		if (streq(manufacturer, "leon") || streq(manufacturer, "invasic"))
			set_be_option("sparc-cpu=leon");
	} else if (streq(cpu, "arm")) {
		setup_arm();
	} else {
		errorf(NULL, "unknown cpu '%s' in target-triple", cpu);
		return false;
	}
	return true;
}

static bool pass_options_to_firm_be(void)
{
	switch (target.object_format) {
	case OBJECT_FORMAT_ELF:     set_be_option("objectformat=elf");    break;
	case OBJECT_FORMAT_MACH_O:  set_be_option("objectformat=mach-o"); break;
	case OBJECT_FORMAT_PE_COFF: set_be_option("objectformat=coff");   break;
	}

	if (profile_generate) {
		driver_add_flag(&ldflags_obst, "-lfirmprof");
		set_be_option("profilegenerate");
	}
	if (profile_use) {
		set_be_option("profileuse");
	}
	if (target.pic_mode > 0) {
		set_be_option("pic=true");
		if (streq(firm_isa, "ia32")) {
			const char *option;
			if (is_darwin_os(target.machine->operating_system)) {
				option = "ia32-pic=mach-o";
			} else {
				option = target.pic_no_plt ? "ia32-pic=elf-noplt"
					                       : "ia32-pic=elf";
			}
			set_be_option(option);
		}
	} else {
		set_be_option("pic=false");
		set_be_option("ia32-pic=none");
	}

	bool res = true;
	/* pass options to firm backend (this happens delayed because we first
	 * had to decide which backend is actually used) */
	for (codegen_option_t *option = codegen_options; option != NULL;
	     option = option->next) {
		char        buf[256];
		const char *opt = option->option;
		/* pass option along to firm backend (except the -m32, -m64 stuff) */
		if (opt[0] < '0' || opt[0] > '9') {
			snprintf(buf, sizeof(buf), "%s-%s", firm_isa, opt);
			if (be_parse_arg(buf) == 0) {
				errorf(NULL, "Unknown codegen option '-m%s'", opt);
				res = false;
				continue;
			}
		}

		/* hack to emulate the behaviour of some gcc spec files which filter
		 * flags to pass to cpp/ld/as */
		static char const *const pass_to_cpp_and_ld[] = {
			"soft-float", "32", "64", "16"
		};
		for (size_t i = 0; i < ARRAY_SIZE(pass_to_cpp_and_ld); ++i) {
			if (streq(pass_to_cpp_and_ld[i], option->option)) {
				snprintf(buf, sizeof(buf), "-m%s", option->option);
				driver_add_flag(&cppflags_obst, buf);
				driver_add_flag(&asflags_obst, buf);
				driver_add_flag(&ldflags_obst, buf);
				break;
			}
		}
	}

	return res;
}

bool target_setup(void)
{
	if (target.machine == NULL)
		target.machine = get_host_machine_triple();

	bool res = setup_firm_isa();
	init_os_support();
	res &= pass_options_to_firm_be();

	const backend_params *be_params = be_get_backend_param();
	multilib_directory_target_triple = NULL;
	if (target.triple == NULL) {
#ifdef MULTILIB_M32_TRIPLE
		if (be_params->machine_size == 32)
			multilib_directory_target_triple = MULTILIB_M32_TRIPLE;
#endif
#ifdef MULTILIB_M64_TRIPLE
		if (be_params->machine_size == 64)
			multilib_directory_target_triple = MULTILIB_M64_TRIPLE;
#endif
	}

	target.byte_order_big_endian = be_params->byte_order_big_endian;
	target.modulo_shift          = be_params->modulo_shift;
	target.float_int_overflow    = be_params->float_int_overflow;
	return res;
}
