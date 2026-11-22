# **00 \- 总览索引（Version A 第一性原理知识追踪）**

本索引文档采用“一原子问题一独立文档”原则追踪 Version A 底层系统核心能力。

# **第一阶段 — 内存、缓存与数据布局物理层（已完成拆分）**

* [01 \- 查询级线性内存池（QueryArena）与堆碎片消除](https://docs.google.com/document/d/1tLqBiCvw5KBe6DCwlZWxzLjd2RHwdV_LuXXGbqyncTY/edit) — 解决单次查询内高频分配导致的堆碎片、锁竞争与超级节点 OOM 问题 \[已完成\]  
* [02 \- 64字节缓存行对齐节点与定长槽位池（GraphNodeSlab）](https://docs.google.com/document/d/1VbyfRQTfAH5S2qKZT5IsG73kpMyMQrf7yKWw7JTcHkA/edit) — 解决指针追踪导致的 L1/L2 Cache Miss、跨缓存行分裂与伪共享问题 \[已完成\]  
* [03 \- 零拷贝存储页视图解码器（ZeroCopyAdjView）](https://docs.google.com/document/d/1LT_7Xk7dxoLLqm0P0Ph9oJ7SMwP0ngQu-vmc9KCeE6E/edit) — 解决从 RocksDB 读取邻接表时的反序列化与冗余内存拷贝问题 \[已完成\]  
* [04 \- 拓扑与属性物理分离（Topology-Property Separation）与延迟物化设计规范](https://docs.google.com/document/d/1Y36-m-3Ip8QLUKoHkWJ2KYFiMKlUSEel9A5HYrBCfm4/edit) — 解决节点属性与边属性混存导致的缓存行污染、I/O 读放大及热路径反序列化开销 \[已完成\]

# **后续阶段 — 存储引擎、无锁并发与 I/O 调度（待拆分建档）**

* 05 \- RocksDB 前缀布隆过滤器（Prefix Bloom Filter）与 SST 布局优化 \[待开始\]  
* 06 \- 后台 Compaction I/O 隔离与写停顿（Write Stall）消除 \[待开始\]  
* 07 \- 单生产者单消费者无锁环形队列（SPSC Ring Buffer）与内存序 \[待开始\]  
* 08 \- 自研分区追加写消息队列（Append-Only Partitioned MQ）与零拷贝游标 \[待开始\]