.section .rodata.gpu_transfer_shader,"a",@progbits
.balign 16
.global gpu_transfer_shader_start
.hidden gpu_transfer_shader_start
gpu_transfer_shader_start:
.incbin ".build/transfer/pq.comp.spv"
.global gpu_transfer_shader_end
.hidden gpu_transfer_shader_end
gpu_transfer_shader_end:
.section .note.GNU-stack,"",@progbits
