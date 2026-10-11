set pagination off
set confirm off
file bin/kernel
set arch riscv:rv64
target remote localhost:1234

echo \n===== [1] at reset (PC should be 0x1000) =====\n
info registers
x/20i $pc

echo \n===== [2] break at 0x80200000 and continue =====\n
break *0x80200000
continue

echo \n===== [3] at kern_entry =====\n
info registers pc sp ra
x/20i $pc

echo \n===== [4] step 3 instructions then inspect stack =====\n
si
si
si
info registers pc sp
x/10x $sp

echo \n===== [5] where is kern_entry symbol =====\n
info address kern_entry
info address bootstacktop

detach
quit
