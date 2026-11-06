# patches/

Patches to third_party/x86recomp (an untracked clone at the pinned commit in tools/bootstrap.sh).
`tools/bootstrap.sh` applies every `x86recomp-*.patch` here with `git apply`, in glob order, onto a
pristine clone (it skips them if the clone already has local changes).

- `x86recomp-h4.patch`, `x86recomp-xbe.patch`: donor-port runtime and XBE loader changes.
- `x86recomp-sse-fastpath.patch`: SSE host fast path under the default MXCSR (bit-exact results,
  no MXCSR sticky flags; `ROTK_SSE_EXACT=1` switches it off). Made with
  `git -C third_party/x86recomp format-patch -1 --stdout` from branch `rotk-sse-fast` there;
  an existing clone takes it with `git -C third_party/x86recomp apply ../../patches/x86recomp-sse-fastpath.patch`.
