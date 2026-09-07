; Photo wallpaper blob: raw 1920x1080x32 pixel data (BGRA byte order matching
; fb 0x00RRGGBB), embedded via incbin (NOT a C array: 8MB would bloat compile
; time/size). Lives in .rodata, so the linker carries it inside kernel.bin and
; GRUB loads it with the image; C accesses it via the symbols below.
; Size contract: exactly 1920*1080*4 = 8294400 bytes - the C side verifies
; photo_wallpaper_end - photo_wallpaper_data before trusting a direct copy.
section .rodata
align 16
global photo_wallpaper_data
global photo_wallpaper_end
photo_wallpaper_data:
incbin "wallpaper.bin"
photo_wallpaper_end:
