/*
  Embedded NNUE networks (Android builds only).

  Assembled into the binary only when NNUE_EMBEDDING is defined, i.e. with the
  Makefile option embed=yes (the default for COMP=ndk). The .incbin paths are
  relative to the make working directory (src/, or temp_builds/<arch>/ for the
  slices of a universal build, where the nets are symlinked).

  Every other target gets an empty object file.
*/

#if defined(NNUE_EMBEDDING) && defined(__ELF__)

    .section .rodata.pikafish_nets, "a", %progbits

    .balign 64
    .globl  pikafish_embedded_net_main
    .globl  pikafish_embedded_net_main_end
    .hidden pikafish_embedded_net_main
    .hidden pikafish_embedded_net_main_end
pikafish_embedded_net_main:
    .incbin "pikafish.nnue"
pikafish_embedded_net_main_end:

    .balign 64
    .globl  pikafish_embedded_net_zero
    .globl  pikafish_embedded_net_zero_end
    .hidden pikafish_embedded_net_zero
    .hidden pikafish_embedded_net_zero_end
pikafish_embedded_net_zero:
    .incbin "pikafish_zero_test.nnue"
pikafish_embedded_net_zero_end:

#endif

#if defined(__ELF__)
    .section .note.GNU-stack, "", %progbits
#endif
