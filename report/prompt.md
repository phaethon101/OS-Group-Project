# Lab1 提示词记录（prompt.md）

> 汇总本实验所有与 AI 协作的提示词与迭代过程。
> 每条包含：用途 / 工具-模型 / 正文 / 结果 / 迭代。
> 报告中的"最终提示词"采用四段式结构：`[PROMPT] / [RELY] / [GUARANTEE] / [SPECIFICATION]`。

---

## 一、练习一：理解程序入口（负责人：2412243-曹珂瑜）

### prompt-01 解释 `la sp, bootstacktop`

- **用途**：理解 `kern/init/entry.S` 中 `la sp, bootstacktop` 的作用与目的
- **工具/模型**：VS Code 插件 / DeepSeek-V4-flash
- **正文**：
  ```text
  讲解在 kern/init/entry.S 中，la sp, bootstacktop 这行代码实现了什么功能，以及它的目的是什么
  ```
- **结果**：成功。得到对 `la` 伪指令展开、`bootstacktop` 含义、8KB 内核栈、以及"为什么是 bootstacktop 而不是 bootstack"的完整讲解（见 `report/images/ai_la_sp_1.png` ~ `ai_la_sp_3.png`）。

### prompt-02 解释 `tail kern_init`

- **用途**：理解 `tail kern_init` 的作用与目的
- **工具/模型**：VS Code 插件 / DeepSeek-V4-flash
- **正文**：
  ```text
  接下来解释一下 tail kern_init 在操作系统内核启动流程中又完成了什么操作，这段代码的目的是什么
  ```
- **结果**：成功。得到 `tail` 展开为 `auipc + jalr x0`、不保存返回地址、与 `call` 的对比、与 `kern_init` 的 `noreturn` 语义吻合等讲解（见 `report/images/ai_tail_1.png` ~ `ai_tail_3.png` 及启动流程图 `ai_boot_flow.png`）。

---

## 二、环境搭建与练习二调试（负责人：2411204-孙唯源）

> 本条与 AI 的完整对话记录见 `report/images/ai_sun_dialogue.jpg`（对话中附件为《组员甲任务指引》）。

### prompt-03 让 AI 给出本任务（环境搭建 + 构建运行 + 练习2 GDB）的完整步骤

- **用途**：一次性拿到"环境搭建 → `make qemu` → 练习2 GDB 调试"的完整操作方案
- **工具/模型**：DeepSeek Harness / deepseek-v4-flash
- **正文**：
  ```text
  （附上《组员甲任务指引：环境搭建 + 构建运行 + 练习2 GDB 调试》文件）
  这个实验怎么做
  ```
- **结果**：成功。AI 按顺序给出完整方案，共 8 节：
  1. 启动 WSL（`wsl -d Ubuntu-22.04`）
  2. 安装 QEMU（`apt install qemu-system-misc`，版本 ≥ 4.1）
  3. 安装 RISC-V 工具链（SiFive 预编译链解压到 `/opt/riscv` + 配 PATH；验证 `gcc -v`）
  4. 放代码并运行（拷到 WSL 家目录 → `make` → `make qemu`，成功标志 `(THU.CST) os is loading ...`）
  5. 练习2：GDB 追踪启动流程（`make debug` + `make gdb`；`info registers`、`x/20i $pc`、`b *0x80200000`、`c`、`si`、`x/10x $sp`）
  6. 练习2 问题怎么答（地址 `0x1000`，固件做初始化、读 hartid、备参后跳 OpenSBI，再到 `0x80200000`）
  7. 截图和交付物（`report/images/` 固定命名 + 练习2 文字记录 + `prompt_甲.md`）
  8. 常见坑（PATH 未配、`make qemu` 卡住属正常、GDB 停在 `0x1000` 属正常等）
- **迭代**：一次提问即得到完整步骤清单，未再迭代。

---

## 三、检验复现与统筹（负责人：2410621-辛梓晗）

### prompt-04 复现并核验练习二 GDB 调试

- **用途**：复现并核验练习二（GDB 从 `0x1000` 到 `0x80200000` 的启动流程追踪），确认输出
- **工具/模型**：OpenCode / deepseek-v4.1-flash
- **正文**：
  ```text
  根据指引指导我复现 GDB 从 0x1000 到 0x80200000 的调试，确认输出
  ```
- **结果**：成功。在 WSL 中按 `make debug` + `make gdb` 复现，确认 GDB 停在复位点 `0x1000`、断点命中 `0x80200000`，得到复位指令、内核入口反汇编与寄存器输出（见 `report/images/gdb_at_1000.png`、`gdb_at_80200000.png`、`gdb_registers.png`）。
- **迭代**：初次为手动逐条执行 GDB 命令；之后**添加合适的自动化复现脚本**——`code/gdb_trace.gdb`（批处理采集复位点/入口的反汇编与寄存器）+ `code/run_gdb.sh`（一键编排：起暂停的 QEMU → 跑批处理 GDB → 收集输出 → 清理），使整段调试可重复复现。
