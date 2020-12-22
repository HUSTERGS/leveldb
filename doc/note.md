开始看代码

根据include顺序开始看
- `include/export.h` 意义不明的东西
- `include/slice.h` 将`const char *`进行封装形成的`Slice`类
- `include/status.h` 定义了几种不同的错误类型
    ```c++
    enum Code {
      kOk = 0,
      kNotFound = 1,
      kCorruption = 2,
      kNotSupported = 3,
      kInvalidArgument = 4,
      kIOError = 5
      };
    ```
- `include/comparator.h`定一个一个比较器的基类，需要具体实现其中的方法
- `include/iterator.h`
- `utils/coding.h/.cc` 关于编码的部分，主要是将数字转化为string或者Slice。
    对于FixedSize一般都是直接将数字的每一个8位取出来，作为一个`uint_8`存放到char类型的缓冲区中
    而对于变长的存储以`EncodeVarint32`为例，主要参考了[这一篇博客](http://mingxinglai.com/cn/2013/01/leveldb-varint32/)
    > Varint是一种紧凑的表示数字的方法。它用一个或多个字节来表示一个数字，值越小的数字使用越少的字节数。这能减少用来表示数字的字节数。比如对于int32类型的数字，一般需要4个byte来表示。但是采用Varint，对于很小的int32类型的数字，则可以用1个byte来表示。当然凡事都有好的也有不好的一面，采用Varint表示法，大的数字则需要5个byte来表示。从统计的角度来说，一般不会所有的消息中的数字都是大数，因此大多数情况下，采用Varint 后，可以用更少的字节数来表示数字信息。
    
    > Varint中的每个byte的最高位bit有特殊的含义，如果该位为 1，表示后续的byte也是该数字的一部分，如果该位为0，则结束。其他的7 个bit都用来表示数字。因此小于128的数字都可以用一个byte表示。大于 128的数字，比如300，会用两个字节来表示：1010 1100 0000 0010
    
    对于Varint就每7位进行保存，然后分别更改对应的第8位
    
    还有就是可能会有Prefix，一般是存放数据的长度，方便后面的解析
`rhs`以及`lhs`分别是right/left hand side的缩写

- `table/block.h/.cc`

  如果没有理解错的话，Block应该存放了重启点的相关信息，比如所有重启点的个数以及每一个重启点的具体信息
  
  **暂时跳过了里面的`Block::Iterator`部分**
  

- `table/format.h/.cc`
  - `BlockHandle` 
    `assert(size_ != ~static_cast<uint64_t>(0));`这他吗是什么用法
  - `Footer`
    `Footer`的长度是固定的
    主要就是保存了两个`BlockHandle`，一个是`metaindex_handle_`一个是`index_handle_`，以及一个魔数(*magic number*)
    
    这两个BlockHandle分别指向了sst文件中的两个index block
  **暂时跳过了`ReadBlock`函数**
  
- `table/block_builder.h/.cc`
  
  由于MemTable在刷入SST的时候，会保持键的有序性，所以以下这种方法可以一定程度上减少键的存储空间
  会有一个叫做restart point的东西。在存储键的时候，不会直接存储键本身，而是存储某一个键与上一个键不同的部分，以及一些元数据，整体格式如下
  
  `[Shared key length] + [Unshared key length] + [Value length] + [Unshared key content] + [Value]`
  
  每隔`block_restart_interval`就会存储一个完整的键，所以可以通过二分查找interval位置的键，然后再在其interval中顺序查找对应的键
  

- `table/filter_block.h/.cc`
  
  建议直接看cc文件中的注释，应该写的还比较清楚。主要需要理解的就是filter和block的对应关系以及filter的具体保存结构

- `util/bloom.cc`

  提供了布隆过滤器的默认实现，需要注意的是所谓的"k个hash函数"的实现，实际上只用了一个hash函数，然后通过迭代的方法来得到后续的hash值

- `table/table.h/.cc`

  其中对于`file_Read(uint64_t offset, size_t n, Slice* result,
  char* scratch)`终于理解了参数中`result`以及`scratch`两个参数的含义了。因为Slice类型本身并不对数据进行管理，而是**指向**某一具体的数据，
  所以需要有一个地方实际存储char *，也就是Slice中的data_成员，这就是`scratch`参数的含义
  