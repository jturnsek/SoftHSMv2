AC_DEFUN([ACX_TSS2],[
 LIBS="$LIBS -ltss2-sys -Wl,-Bstatic -ltss2-tcti-tabrmd -Wl,-Bdynamic"
])