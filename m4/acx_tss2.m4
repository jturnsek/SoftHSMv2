AC_DEFUN([ACX_TSS2],[
 LIBS="$LIBS -ltss2-sys -ltss2-tcti-tabrmd"
 LDFLAGS="$LDFLAGS -Wl,-Bstatic"
])