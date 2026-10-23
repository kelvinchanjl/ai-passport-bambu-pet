/* Embeds pet_frames.bin (RGB565 little-endian frames) 4-byte aligned in flash rodata. */
    .section .rodata.pet_frames
    .balign 4
    .global pet_frames_bin
pet_frames_bin:
    .incbin "pet_frames.bin"
    .balign 4
