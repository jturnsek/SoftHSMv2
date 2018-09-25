AC_DEFUN([ACX_TSS2],[
 LIBS="$LIBS -ltss2-sys -lgio-2.0 -lgobject-2.0 -lglib-2.0 -lgmodule-2.0 -lgthread-2.0 -l:libtss2-tcti-tabrmd.a"
])