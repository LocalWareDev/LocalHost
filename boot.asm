[BITS 16]
[ORG 0x0]

start:
    mov si, message
    mov dx, 0x3F8
.loop:
    lodsb
    cmp al, 0
    je .done
    out dx, al
    jmp .loop
.done:
    hlt

message: db "Hello from real assembly!", 10, 0