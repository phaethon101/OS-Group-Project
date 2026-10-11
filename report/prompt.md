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

## 二、环境与运行线（负责人：2411204-孙唯源）

### prompt-03 修复 `make qemu` 无法进入内核（QEMU/OpenSBI 版本差异）

- **用途**：让 `make qemu` 在本机 QEMU 7.0 + OpenSBI 1.0 上正常运行并进入内核
- **工具/模型**：<!-- 待填：孙唯源的 AI 工具与模型 -->
- **现象**：`make qemu` 停在 OpenSBI 横幅，没有内核输出；横幅中显示
  `Domain0 Next Address : 0x0000000000000000`
- **正文**：
  ```text
  <待填：实际提问原文；核心是"make qemu 停在 OpenSBI 横幅没有内核输出，横幅里 Domain0 Next Address 是 0x0，为什么？" >
  ```
- **AI 诊断**：Makefile 的 `qemu` 目标用 `-device loader,file=ucore.img,addr=0x80200000`，它只把内核**放入内存**，并不会让 OpenSBI 跳过去；新版 OpenSBI 的 next address 是 `0x0`（老版本默认 `0x80200000`，所以指导书里能跑），因此内核从未被执行。
- **修复**：把 `-device loader,...` 改为 `-kernel`：
  ```bash
  sed -i 's|-device loader,file=$(UCOREIMG),addr=0x80200000|-kernel $(UCOREIMG)|g' Makefile
  ```
- **结果**：成功。OpenSBI 横幅中 `Domain0 Next Address : 0x0000000080200000`，并打印 `(THU.CST) os is loading ...`（见 `report/images/qemu_success.png`）。
- **迭代**：第 1 次用 `-device loader` 失败 → 定位为 QEMU/OpenSBI 版本行为差异 → 第 2 次改用 `-kernel` 成功。

---

## 三、练习二：GDB 调试启动流程（负责人：2411204-孙唯源）

### prompt-04 <标题>

- **用途**：指导 GDB 调试步骤与命令（`make debug` / `make gdb`、`x/20i $pc`、断点 `*0x80200000` 等）
- **工具/模型**：<!-- 待填 -->
- **正文**：
  ```text
  <待填：实际提问原文>
  ```
- **结果**：
- **迭代**：

---

## 四、报告撰写（负责人：2410621-辛梓晗）

### prompt-05 <标题>

- **用途**：整理报告结构、术语与表述
- **工具/模型**：<!-- 待填：辛梓晗的 AI 工具与模型 -->
- **正文**：
  ```text
  <待填：实际提问原文>
  ```
- **结果**：
- **迭代**：
