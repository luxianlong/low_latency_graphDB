# **03 \- 零拷贝存储页视图解码器（ZeroCopyAdjView）设计规范**

> 本文档与 `include/low_latency_graphdb/adjacency_view.hpp` 的当前实现保持一致。
> **范围说明：** 本文只描述 View 本身的设计与邻接目标数组（`targets`）的解码。cf_topo 行中紧随其后的 `EdgeLightMeta` 数组属于另一个主题（见文档 04：拓扑与属性分离），不在本文讨论范围内。

# **1\. Background（背景与通俗概念解析）**

## **1.1 业务与访问模式背景（Workload Context）**

### **1.1.1 海量冷温顶点与超长邻接表的扫描特征**

* **海量长尾顶点访问：**在大型图计算系统执行 2\~4 跳图遍历（BFS/DFS）时，除了热点顶点能常驻在计算层自定义内存池（`FixedSlabPool`）外，绝大多数冷温顶点以及包含数千至百万条边的超级节点（Hub）均存储在底层 KV 存储引擎（RocksDB）中。每次跳跃扩展邻居时，计算算子都需要从 RocksDB BlockCache 读取对应的邻接表字节流。

### **1.1.2 高频跳变下“读放大”与 CPU 周期预算的冲突**

* **极其苛刻的 CPU 周期预算：**单次图查询可能访问数万个顶点，端到端 SLA 延迟预算仅有 1ms 至 10ms。分配给单次图跳跃（解析并遍历一个顶点的所有出边）的 CPU 周期被压缩到数十个时钟周期内，任何微小的反序列化开销或内存分配都会直接导致 SLA 超时。

## **1.2 传统存储页反序列化在热路径上的四大根因问题（Problem Statement）**

### **1.2.1 问题一：跨存储边界的重复内存拷贝（memcpy）消耗 CPU 带宽**

* **概念白话解读：**RocksDB 已经将 SSTable 数据块解压并存放在进程内存的 BlockCache 中。如果每次遍历都把字节流解析并拷贝到新建的 `std::vector<uint64_t>` 或 `std::string`，就是在同一进程内存空间里搬运数据。
* **物理后果：**大量 `memcpy` 指令占满内存总线带宽，浪费有效计算周期。

### **1.2.2 问题二：临时容器分配引发延迟抖动与缓存污染（Cache Pollution）**

* **概念白话解读：**现代 CPU 的 L1 数据缓存通常仅 32KB\~48KB，L2 约 512KB\~1MB。每次图跳跃如果在堆上新建临时邻居容器，不仅触发通用分配器（tcmalloc/jemalloc）的锁与分配开销，还会把这些副本强行装入 L1/L2，驱逐原本驻留的热点节点与遍历状态。

### **1.2.3 问题三：无生命周期保护的裸指针引用引发悬空指针（Dangling Pointer）崩溃**

* **概念白话解读：**若直接持有 RocksDB 内部 Block 缓存的原始内存地址，后台线程在内存不足时执行 LRU 淘汰或 Block 替换，该块内存会被释放或重用，正在遍历的线程会读到脏数据甚至触发 Segmentation Fault。

### **1.2.4 问题四：非对齐内存强转读取触发硬件异常或未定义行为（UB）**

* **概念白话解读：**RocksDB Data Block 中 Value 的起始地址取决于前置 Key 的长度，往往不是 8 字节对齐（例如 `0x1003`）。直接执行 `*reinterpret_cast<const uint64_t*>(ptr)` 违反严格别名与对齐规范，在 x86 上是 UB，在 ARM64 等严格对齐架构上可能触发总线异常（SIGBUS）。

## **1.3 底层物理原理与系统边界（Constraints & Boundaries）**

### **1.3.1 底层原理 1：16 字节视图的寄存器级值传递（核心性能设计）**

* **概念白话解读 — System V AMD64 ABI：**在 64 位 Linux/macOS 的 System V AMD64 ABI 中，**大小不超过 16 字节且可平凡拷贝（trivially copyable）**的结构体，会被拆成两个 INTEGER 类寄存器传参或返回，不经过栈。超过 16 字节的结构体则通过内存（栈）传递。
* **本项目的约束：**`ZeroCopyAdjView` 必须始终保持 **`sizeof == 16`** 且 **trivially copyable**，并由 `static_assert` 在编译期强制：

  ```cpp
  static_assert(sizeof(ZeroCopyAdjView) == 16);
  static_assert(std::is_trivially_copyable_v<ZeroCopyAdjView>);
  ```
* **内存布局（16 字节）：**

  | 偏移 | 大小 | 字段 | 含义 |
  |---|---|---|---|
  | 0 | 8B | `const char* data_` | 指向 RocksDB 返回的原始只读字节（Header 起点） |
  | 8 | 4B | `uint32_t count_` | 解码并校验过的邻接边数量 |
  | 12 | 4B | `uint32_t reserved_` | 预留，保证 16 字节；用途见 §2.4 |

* **推导而非存储：**`targets` 指针、对齐状态等都不单独存储，而是由 `data_` 与 `count_` 在使用时计算（`data_ + kHeaderSize`）。这样视图始终是最小的“胖指针”。
* **不能做的事：**为了让视图可按值传递，**不能删除拷贝构造/赋值、不能自定义析构**，否则不再 trivially copyable，ABI 会退化为按引用（内存）传递。生命周期约束改由 RAII 作用域与约定保证（见 §2.3）。

### **1.3.2 底层原理 2：原位字节流映射与编译期优化的定长 std::memcpy**

* **概念白话解读：**为安全提取非对齐地址处的整数，使用 `std::memcpy(&val, ptr, sizeof(val))`。GCC/Clang 在编译期识别长度固定的 `memcpy`，在 x86-64 / ARM64 上展开为单条支持非对齐访问的 `MOV`/`LDR` 指令，无函数调用开销，同时规避 UB。

### **1.3.3 底层原理 3：引用计数物理页锁定（Page Pinning）**

* **概念白话解读：**RocksDB 的 `PinnableSlice` 在 BlockCache 命中时将 Block 的原子引用计数加 1，只要计数大于 0，LRU 淘汰就不能释放或覆写该块。`PinnableSlice` 析构后引用计数减 1，Block 重新可淘汰。

### **1.3.4 RocksDB 原生提供能力（Storage Layer Boundary）**

* **存储层职责：**磁盘 SSTable 物理块读取、解压、CRC 校验，并通过 `rocksdb::PinnableSlice` 暴露只读连续字节指针与引用计数锁。
* **存储层缺口：**RocksDB 将 Value 视为不透明的二进制字节流，不了解其内部有多少条边、ID 占多少字节，也没有越界防护和标准 C++ 视图封装。

### **1.3.5 自研引擎设计组件（Compute Layer Boundary）**

* **计算层职责：**`ZeroCopyAdjView` 解析二进制 Header，进行防溢出截断校验，实现非对齐安全读取，并向遍历算子暴露零拷贝随机访问与 `std::span` 连续迭代接口。
* **与 RocksDB 解耦：**核心类只接收 `(const char* data, size_t size)`，**不依赖 `<rocksdb/...>` 头文件**。`PinnableSlice` 到 View 的绑定由存储层完成（`AdjacencyBatch::bind` 内部调用 `view.reset(slice.data(), slice.size())`）。

# **2\. High-Level Design（总体架构设计）**

## **2.1 存储页二进制字节布局（Binary Wire Format）**

View 解析的连续字节流采用 **4 字节 Header 加定长元素数组**的布局（native-endian）：

```
+-------------------+---------------------+---------------------+--...--+------------------------+
| 0x00 - 0x03       | 0x04 - 0x0B         | 0x0C - 0x13         |  ...  | 0x04+(N-1)*8 .. +7     |
+-------------------+---------------------+---------------------+--...--+------------------------+
| count (uint32)    | target_node[0]      | target_node[1]      |  ...  | target_node[N-1]       |
| Header：邻接边数 N | NodeId (uint64)     | NodeId (uint64)     |       | NodeId (uint64)        |
+-------------------+---------------------+---------------------+--...--+------------------------+
```

> 在 cf_topo 的完整行中，`target_node[N-1]` 之后还跟随 `EdgeLightMeta[N]`。该部分不属于本文范围，参见文档 04。View 只要求 Header 与 `targets` 区间完整可读。

**数值计算示例：**
假设某顶点有 3 条出边（N = 3），`targets` 部分总长 `4 + 3 * 8 = 28` 字节：

* Header：`[0x00 - 0x03]` 存 `uint32_t(3)`；
* 读取 `index = 2` 的邻居：偏移 `kHeaderSize + 2 * sizeof(NodeId) = 4 + 16 = 20 (0x14)`，读取 `[0x14 - 0x1B]` 共 8 字节。

**对齐的含义：**Header 只有 4 字节，所以 `targets` 区间的起始地址是 `data + 4`。只有当 `data + 4` 是 8 的倍数（即 `data % 8 == 4`）时，`targets` 才是自然对齐，才能暴露 `std::span<const NodeId>`；否则退回逐元素 `memcpy`。

## **2.2 跨存储-计算层生命周期与零拷贝调用链路（Lifecycle Flow）**

```
+------------------+     +------------------------+     +-------------------+     +--------------------------+
| RocksDB Storage  |     | rocksdb::PinnableSlice |     | ZeroCopyAdjView   |     | Graph Traversal Operator |
+------------------+     +------------------------+     +-------------------+     +--------------------------+
        | 1. Get/MultiGet(..., &slice)  |                          |                          |
        |------------------------------>|                          |                          |
        |  (BlockCache Pin: ref++)      |                          |                          |
        |                               | 2. view.reset(data,size) |                          |
        |                               |------------------------->|                          |
        |                               |   (16B: ptr + count)     | 3. 校验 Header/截断      |
        |                               |                          |------------------------->|
        |                               |                          |  4. target(i) / targets()|
        |                               | 5. 作用域结束            |                          |
        |<------------------------------|<-------------------------|<-------------------------|
        |  (Unpin: ref--)               |                          |                          |
```

* **阶段一：点查命中与物理页锁定。**图算子调用 `Get`/`MultiGet`，命中 BlockCache 后，Block 引用计数加 1。批量路径只接受 `IsPinned()` 为真的结果（未钉住的 value 视为未找到），以保证零拷贝语义成立。
* **阶段二：16 字节视图绑定。**`view.reset(slice.data(), slice.size())` 只记录指针与解析出的 `count_`，不发生堆分配或数据拷贝。
* **阶段三：Header 解析与截断校验。**`reset()` 通过定长 `memcpy` 读出 `count`，并用安全的乘法/加法检查（见 §2.3）确认 payload 完整。
* **阶段四：原位随机访问与连续扫描。**`target(i)` 逐项读取，或在对齐时用 `targets()` 获得 `std::span`，直接在 BlockCache 页上读取。
* **阶段五：作用域结束与解锁。**`PinnableSlice`（或 `AdjacencyBatch`）析构后，Block 引用计数减 1。

### **2.2.1 与 Slab 池、Query Arena 的关系**

* **View 不存数据**：只指向 BlockCache 中的字节，生命周期短于 `PinnableSlice`。
* **回填 Slab 池**：未缓存顶点解码后，`HotTier::populate_if_missing(local, view, write_seq)` 把邻居**复制**进 `GraphNodeSlab` 以便后续查询命中（有并发写校验）。这是一次有意的一次性拷贝，不属于“零拷贝”段。
* **池满回退**：当 Slab 池或本地 ID 容量耗尽时，`populate_if_missing` 返回 `nullptr`，遍历直接用 View 原位展开邻居，不入缓存。该查询仍然正确，仅该顶点不被缓存。
* **Query Arena**：只存 BFS 的 frontier（`LocalNodeId` 数组），不存邻接数据，也不存边信息。

## **2.3 关键工程权衡与失效模式防护（Trade-offs & Failure Modes）**

* **权衡一：View 不拥有内存，生命周期由约定而非类型保证。**为保持 16 字节 trivially copyable 并可按值传递，View 允许拷贝。因此**严禁**将 View 跨线程、跨异步任务或长期保存；它只能在持有对应 `PinnableSlice`/`AdjacencyBatch` 的同步作用域内使用。以此换取 0 拷贝、0 堆分配与寄存器传参。
* **权衡二：单点访问 `memcpy` vs. 批量访问对齐检查。**
  * `target(idx)`：一律使用定长 `memcpy`，在任意对齐下都安全。
  * `targets()`：返回 `std::span<const NodeId>` 前要求 `targets` 起点按 `alignof(NodeId)` 对齐；不满足则抛 `std::logic_error`。调用方应先判断 `is_zero_copy()`。
* **失效防护：损坏数据导致的整数溢出与越界。**`count` 来自磁盘数据，可能被损坏（例如 `0xFFFFFFFF`）。`reset()` 使用 `checked_multiply`（乘法前检测溢出）与 `checked_add`（加法前检测溢出）计算所需总字节数，若溢出或超过 `size` 则抛 `std::invalid_argument`，在构造阶段就拦截，不会产生越界读取。
  * 说明：早期方案用除法 `raw_count > (size - HEADER) / sizeof(NodeId)` 来规避溢出。当前实现用显式的“检查后计算”达到同样效果，两者都能消除溢出绕过。

## **2.4 16 字节设计的可扩展性（Future: 变长视图内容）**

16 字节只约束 **View 对象本身**，不约束它所指向的字节流的大小或结构。将来可以在不改变 View 大小的前提下扩展内容：

* **`reserved_`（4 字节）可承载**：布局版本号、元素宽度/类型标志、压缩或编码方式标志等。
* **变长元素**：元素个数仍由 `count_` 给出；若引入变长编码（如 delta/varint 压缩的邻接表），可用 `reserved_` 标记编码方式，并把“字节长度”放入 Header 或由解码器顺序推进，而不增加 View 的字段。
* **派生而非存储**：所有扩展信息优先放在 Header 或 `reserved_` 中，由访问器按需计算，避免把 View 撑大到 16 字节之上而丢失寄存器传参。
* **约束不变**：任何改动都必须继续通过 `sizeof == 16` 与 `is_trivially_copyable` 的 `static_assert`。

# **3\. Detail Design（详细设计与核心代码）**

## **3.1 核心 C++20 实现（节选，以 `adjacency_view.hpp` 为准）**

```cpp
class ZeroCopyAdjView {
public:
    using NodeId = std::uint64_t;
    static constexpr std::size_t kHeaderSize = sizeof(std::uint32_t);

    ZeroCopyAdjView() = default;

    ZeroCopyAdjView(const char* data, std::size_t size) { reset(data, size); }

    // 不拷贝 payload。解析 Header 并校验完整性，失败则抛异常且视图保持为空。
    void reset(const char* data, std::size_t size) {
        data_ = nullptr;
        count_ = 0;
        if (data == nullptr || size < kHeaderSize) {
            throw std::invalid_argument("ZeroCopyAdjView: truncated header");
        }
        const std::uint32_t count = read_u32(data);        // 定长 memcpy
        const std::size_t target_bytes = checked_multiply(count, sizeof(NodeId));
        const std::size_t expected = checked_add(kHeaderSize, target_bytes);
        if (expected > size) {
            throw std::invalid_argument("ZeroCopyAdjView: truncated adjacency payload");
        }
        data_ = data;
        count_ = count;
    }

    [[nodiscard]] std::size_t size() const noexcept { return count_; }

    // targets() 可用的条件：targets 起点按 alignof(NodeId) 对齐。
    [[nodiscard]] bool is_zero_copy() const noexcept {
        return data_ != nullptr && is_aligned(targets_ptr(), alignof(NodeId));
    }

    [[nodiscard]] NodeId target(std::size_t index) const {   // 任意对齐都安全
        check_index(index);
        NodeId value{};
        std::memcpy(&value, targets_ptr() + index * sizeof(NodeId), sizeof(value));
        return value;
    }

    [[nodiscard]] std::span<const NodeId> targets() const {   // 仅对齐时可用
        require_aligned();
        return {reinterpret_cast<const NodeId*>(targets_ptr()), count_};
    }

private:
    [[nodiscard]] const char* targets_ptr() const noexcept { return data_ + kHeaderSize; }

    const char*   data_{nullptr};       // 8B
    std::uint32_t count_{0};            // 4B
    [[maybe_unused]] std::uint32_t reserved_{0};  // 4B，预留
};

static_assert(sizeof(ZeroCopyAdjView) == 16);
static_assert(std::is_trivially_copyable_v<ZeroCopyAdjView>);
```

> 注：实际实现里 `reset()` 的 `expected` 目前还包含 meta 数组的字节数，因为 cf_topo 行里 meta 紧跟其后。本文只描述 `targets` 部分；meta 属于文档 04 的范围。

### **3.1.1 在遍历中的标准用法（按值传递）**

```cpp
// 按值传递：16 字节，走两个寄存器
void expand_view(ZeroCopyAdjView view, ...);
const GraphNodeSlab* populate_if_missing(LocalNodeId local, ZeroCopyAdjView view, std::uint64_t seq);

// 存储层绑定：PinnableSlice -> View（不拷贝）
batch.bind(i, state.view);   // 内部 view.reset(values_[i].data(), values_[i].size())

if (state.view.is_zero_copy()) {
    for (auto target : state.view.targets()) { /* ... */ }
} else {
    for (std::size_t j = 0; j < state.view.size(); ++j) {
        auto target = state.view.target(j);
    }
}
```

## **3.2 核心 C++ 底层语法与硬件协作原语逐项解读**

* **16 字节 trivially copyable 类型与寄存器传参：**按值传递一个 `ZeroCopyAdjView` 等价于传两个 64 位整数。前提是类型没有用户自定义的拷贝/移动/析构，也不含虚函数。
* **`rocksdb::PinnableSlice` 与 RAII：**构造并命中缓存时递增引用计数，析构时递减。View 自身不参与引用计数，只能在 `PinnableSlice` 存活期间使用。
* **定长 `std::memcpy`：**解引用非自然对齐指针是 UB。`memcpy(&v, p, sizeof(v))` 在编译期被识别为单条非对齐加载，既无 UB 也无性能损失。
* **`std::span<const NodeId>`：**非所有权连续数组视图（指针加长度），适配 range-for 与 STL 只读算法，无堆分配。
* **`[[nodiscard]]` 与 `noexcept`：**只读 Getter 标记 `[[nodiscard]]`；不抛异常的轻量方法声明 `noexcept`，减少异常展开表开销。
* **`checked_multiply` / `checked_add`：**在做乘法/加法前，用 `max / left` 与 `max - left` 判断是否会溢出，避免无符号回绕绕过长度校验。
* **`uintptr_t` 对齐检测：**将指针转为整数后对 `alignof(NodeId)` 取模，仅在 8 的倍数时才允许 `reinterpret_cast` 成 `NodeId*` 并暴露 `span`。
* **`[[maybe_unused]]`：**`reserved_` 目前未被读取，用该属性避免未使用字段警告，同时保留 16 字节布局。

## **3.3 系统边界职责总结（RocksDB vs 自研引擎）**

* **RocksDB 职责一：**SSTable 压缩块读取、解压与校验。
* **RocksDB 职责二：**BlockCache 原子引用计数锁定（Page Pinning），通过 `PinnableSlice` 递增/递减。
* **RocksDB 职责三：**向上层只暴露原始连续内存地址与字节长度（`data()` / `size()`）。
* **自研引擎职责一：**二进制协议解析与防溢出完整性校验（`reset()`）。
* **自研引擎职责二：**非对齐内存安全读取（定长 `memcpy`），消除总线异常与 UB。
* **自研引擎职责三：**运行时越界拦截与 `std::span` 适配（`target(i)` / `targets()`）。
* **自研引擎职责四：**保持 View 为 16 字节 trivially copyable，并通过作用域约定保证 View 不会超出底层 `PinnableSlice` 的生命周期。
