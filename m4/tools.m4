# KDUMP_TRY_LINK(LIBRARIES)
# ---------------------------------------------------------
# Try to link the existing test source file with additional
# libraries.
# Set kdump_res to "yes" on success, else set it to "no".
# Standard error output is saved to conftest.linkerr.
AC_DEFUN([KDUMP_TRY_LINK],[dnl
kdump_save_LIBS="$LIBS"
kdump_save_ac_link="$ac_link"
LIBS="$1 $LIBS"
ac_link="$ac_link 2>conftest.linkerr"
AC_LINK_IFELSE([], kdump_res=yes, [kdump_res=no
  $2])
LIBS="$kdump_save_LIBS"
ac_link="$kdump_save_ac_link"
])# KDUMP_TRY_LINK

AC_DEFUN([KDUMP_REPORT_LINKERR],[dnl
AS_ECHO("$as_me:$LINENO: all linker errors") >&AS_MESSAGE_LOG_FD
cat conftest.linkerr >&AS_MESSAGE_LOG_FD
])# KDUMP_REPORT_LINKERR

AC_DEFUN([KDUMP_TRY_LINK_UNDEF],[dnl
kdump_save_LDFLAGS="$LDFLAGS"
LDFLAGS="-z undefs $2 $LDFLAGS"
KDUMP_TRY_LINK($1, KDUMP_REPORT_LINKERR)
LDFLAGS="$kdump_save_LDFLAGS"
])# KDUMP_TRY_LINK_UNDEF

AC_DEFUN([KDUMP_DIS_ASM_CHECK_UNDEF],[dnl
AC_REQUIRE([AC_PROG_EGREP])dnl
AC_MSG_CHECKING([whether disassembler requires $1])
KDUMP_TRY_LINK($DIS_ASM_LIBS)
AS_ECHO("$as_me:$LINENO: matching linker errors") >&AS_MESSAGE_LOG_FD
AS_IF([$EGREP "@<:@^A-Za-z0-9_@:>@($2)" conftest.linkerr >&AS_MESSAGE_LOG_FD],
  [AC_MSG_RESULT(yes)
   DIS_ASM_LIBS="$DIS_ASM_LIBS $1"
   KDUMP_TRY_LINK_UNDEF($DIS_ASM_LIBS)
   AS_IF([test yes != "$kdump_res"],
     [AC_MSG_FAILURE([Link fails with $1])])],
  [AC_MSG_RESULT(no)])dnl
])# KDUMP_DIS_ASM_CHECK_UNDEF

AC_DEFUN([KDUMP_DIS_ASM_LIBS],[[dnl determine disassembler libraries
DIS_ASM_LIBS=-lopcodes
AC_LANG_CONFTEST([AC_LANG_PROGRAM(
  [#include <dis-asm.h>],
  [disassembler(bfd_arch_i386, FALSE, bfd_mach_x86_64, NULL);])])
dnl ignore undefined symbols from missing linker dependencies
AC_MSG_CHECKING([for disassembler in $DIS_ASM_LIBS])
KDUMP_TRY_LINK_UNDEF($DIS_ASM_LIBS, [[-Wl,--require-defined=disassembler]])
AC_MSG_RESULT($kdump_res)
AS_IF([test yes = "$kdump_res"], [dnl
  KDUMP_DIS_ASM_CHECK_UNDEF(-lbfd, bfd_)
  KDUMP_DIS_ASM_CHECK_UNDEF(-lsframe, sframe_)
  KDUMP_DIS_ASM_CHECK_UNDEF(-liberty, htab_create|splay_tree_new)
  KDUMP_DIS_ASM_CHECK_UNDEF(-lz, inflate)
  KDUMP_DIS_ASM_CHECK_UNDEF(-lzstd, ZSTD_)
  KDUMP_DIS_ASM_CHECK_UNDEF(-ldl, dlopen)
  AS_IF([test yes != "$kdump_res"],
    [KDUMP_REPORT_LINKERR]
    [AC_MSG_FAILURE([Tried everything, still cannot link disassembler.])])
  AC_SUBST(DIS_ASM_LIBS)
], false)dnl
]])# KDUMP_DIS_ASM_LIBS

AC_DEFUN([KDUMP_DIS_ASM],[dnl determine disassembler options
AC_CHECK_HEADERS(dis-asm.h, [],
  [AC_MSG_ERROR([Disassembler headers not found])])
AC_MSG_CHECKING([whether disassembler supports syntax highlighting])
AC_COMPILE_IFELSE([AC_LANG_SOURCE([
#include <dis-asm.h>
void fn(struct disassemble_info *info, void *stream,
	fprintf_ftype fprintf_func, fprintf_styled_ftype fprintf_styled_func)
{
    init_disassemble_info(info, stream, fprintf_func, fprintf_styled_func);
}
])],
  [dnl
AC_MSG_RESULT(yes)
AC_DEFINE(DIS_ASM_STYLED_PRINTF, [1],
  [Define if init_disassemble_info() has a printf_styled_func parameter])],
  [AC_MSG_RESULT(no)])
KDUMP_DIS_ASM_LIBS
])# KDUMP_DIS_ASM

AC_DEFUN([KDUMP_TOOL_KDUMPID],[dnl enable/disable kdumpid build
AC_ARG_ENABLE(kdumpid,
  [AS_HELP_STRING(--disable-kdumpid,
    [do not build kdumpid])],
  [],
  [enable_kdumpid=yes])
AS_IF([test no != "$enable_kdumpid"],
  [AS_IF(KDUMP_DIS_ASM, [],
    [AC_MSG_FAILURE(
      [disassembler test failed (--disable-kdumpid to disable)])]
    )])
AM_CONDITIONAL(BUILD_KDUMPID, [test yes = "$enable_kdumpid"])
])# KDUMP_TOOL_KDUMPID
