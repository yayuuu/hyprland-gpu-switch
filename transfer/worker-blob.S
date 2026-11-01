.section .rodata.gpu_transfer_worker,"a",@progbits
.balign 16
.global gpu_transfer_worker_start
.hidden gpu_transfer_worker_start
gpu_transfer_worker_start:
.incbin GPU_TRANSFER_WORKER_PATH
.global gpu_transfer_worker_end
.hidden gpu_transfer_worker_end
gpu_transfer_worker_end:
.section .note.GNU-stack,"",@progbits
